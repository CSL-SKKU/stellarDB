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

#include <errno.h>
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

/* The merge the sources imply, newest first, deduped by key. */
static void build_expected(centree_node *srcs, int nb_srcs) {
  exp_cap = 0;
  for (int i = 0; i < nb_srcs; i++)
    exp_cap += srcs[i]->value.slab->nb_max_items;
  free(exp_list);
  exp_list = calloc(exp_cap, sizeof(*exp_list));
  nb_exp = 0;
  for (int i = 0; i < nb_srcs; i++) {
    exp_src = srcs[i]->value.slab;
    exp_rank = i;
    subtree_forall_entries(exp_src->subtree, exp_cb, NULL);
  }
  qsort(exp_list, nb_exp, sizeof(*exp_list), cmp_exp);
}

static void collect_got(struct slab *n) {
  got_cap = n->nb_max_items;
  free(got_list);
  got_list = calloc(got_cap, sizeof(*got_list));
  nb_got = 0;
  subtree_forall_entries(n->subtree, got_cb, NULL);
}

/* N must hold exactly the expected merge, byte for byte. */
static void verify_merged(struct slab *n, const char *when) {
  size_t want = 0;
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
  check(want == nb_got, "[%s] N holds %zu entries, expected %zu", when, nb_got,
        want);
  check(n->nb_items == nb_got, "[%s] N's nb_items is %zu, entries %zu", when,
        n->nb_items, nb_got);
  check(atomic_load(&n->full) == 1, "[%s] N is not marked full", when);
}

static void check_one_cold_build(const struct prune_candidate *c) {
  struct prune_build build;
  centree_node srcs[2];
  int error = prune_build_cold(c, &build);

  if (error) {
    check(false, "prune_build_cold failed with %d", error);
    prune_build_discard(&build);
    return;
  }
  srcs[0] = c->inner;
  srcs[1] = c->outer;
  build_expected(srcs, 2);
  collect_got(build.slab);
  verify_merged(build.slab, "cold");
  check(build.count == nb_got, "N's count is %zu, entries %zu", build.count,
        nb_got);
  check(atomic_load(&build.node->child_flag) == 1,
        "N's node is not marked fully split");
  prune_build_discard(&build);
  check(build.slab == NULL, "discard left the build populated");
}

static void check_empty_build(const struct prune_candidate *c) {
  struct prune_build build;
  int error = prune_build_begin(c, c->outer, &build);

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
    cov_side[c.side]++;
    if (!c.up) cov_history_root++;
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

/* ------------------------------------------- reads must not change */

static int *snap_found;
static uint64_t *snap_value;
static uint64_t snap_hi;

static void snapshot_reads(uint64_t hi) {
  snap_hi = hi;
  free(snap_found);
  free(snap_value);
  snap_found = calloc(hi, sizeof(*snap_found));
  snap_value = calloc(hi, sizeof(*snap_value));
  for (uint64_t k = 0; k < hi; k++) {
    read_back_quiet(k);
    snap_found[k] = rb_found;
    snap_value[k] = rb_value;
  }
}

static void verify_reads(const char *when) {
  size_t bad = 0;

  for (uint64_t k = 0; k < snap_hi; k++) {
    read_back_quiet(k);
    if (rb_found == snap_found[k] && (!rb_found || rb_value == snap_value[k]))
      continue;
    if (bad < 5)
      check(false, "[%s] key %lu: found %d->%d, value %lu->%lu", when, k,
            snap_found[k], rb_found, snap_value[k], rb_value);
    bad++;
  }
  check(bad == 0, "[%s] %zu of %lu keys changed", when, bad, snap_hi);
  if (!bad) printf("  %-34s %lu keys unchanged\n", when, snap_hi);
}

/* ------------------------------------- freeze, hot copy, history link */

static uint64_t leaf_keys[8192];
static size_t nb_leaf_keys;

static void leaf_key_cb(uint64_t key, uint32_t slot, void *data) {
  (void)data;
  if (slot & (1u << 31)) return;
  if (nb_leaf_keys < 8192) leaf_keys[nb_leaf_keys++] = key;
}

/*
 * One real prune, stopped in the middle so the intermediate state can be
 * inspected. After the history link the two trees deliberately disagree (N is
 * in the history chain, the triple is still in the routing tree) and a writer
 * whose key routes to the frozen leaf is parked. The splice publishes N,
 * retires the triple and releases that writer.
 */
static void check_prune_link(void) {
  size_t cap = tnt_get_node_count() + 8;
  centree_node *order = calloc(cap, sizeof(*order));
  centree_node srcs[3];
  struct prune_candidate best;
  struct prune_build build;
  size_t nb_order = 0;
  bool have = false;
  int error;

  collect_routing(routing_root(), order, cap, &nb_order);
  for (size_t i = 0; i < nb_order; i += 2) {
    struct prune_candidate c;

    if (!prune_select(order[i], &c)) continue;
    /*
     * Prefer the triple whose outer is the history root, so the D == NULL
     * branch of the history link runs; failing that, one with something to
     * carry over.
     */
    if (!have || (!c.up && best.up) ||
        (!c.up == !best.up && c.cold_bound > best.cold_bound)) {
      best = c;
      have = true;
    }
  }
  free(order);
  if (!have) {
    check(false, "no candidate to prune");
    return;
  }

  /*
   * A live key that routes to the leaf. It gets rewritten with its own value,
   * so the read snapshot stays valid across the prune.
   */
  nb_leaf_keys = 0;
  subtree_forall_entries(best.leaf->value.slab->subtree, leaf_key_cb, NULL);
  uint64_t blocked_key = 0, blocked_value = 0;

  for (size_t i = 0; i < nb_leaf_keys && !blocked_key; i++) {
    read_back_quiet(leaf_keys[i]);
    if (rb_found) {
      blocked_key = leaf_keys[i];
      blocked_value = rb_value;
    }
  }
  check(blocked_key != 0, "no readable key routes to the leaf");
  check(!blocked_key || leaf_for_key(blocked_key) == best.leaf,
        "key %lu does not route to the leaf", blocked_key);

  /* The in-order sequence the splice has to produce. */
  size_t cap2 = tnt_get_node_count() + 8;
  centree_node *before = calloc(cap2, sizeof(*before));
  centree_node *want = calloc(cap2, sizeof(*want));
  centree_node *after = calloc(cap2, sizeof(*after));
  size_t nb_before = 0, nb_want = 0, nb_after = 0;

  collect_routing(routing_root(), before, cap2, &nb_before);
  snapshot_reads(nb_keys + nb_keys / 2);

  /* Held across both halves, as the real caller must. */
  tnt_maintenance_lock();
  error = prune_freeze_and_link(&best, &build);
  check(error == 0, "prune_freeze_and_link failed with %d", error);
  if (error) {
    tnt_maintenance_unlock();
    return;
  }

  /* The three history rewires, and nothing else. */
  check(centree_lu_parent(build.node) == best.up, "N->lu_parent is wrong");
  check(build.node->lu_child[best.side] == best.star,
        "N->lu_child[side] is not star");
  check(build.node->lu_child[!best.side] == best.sib,
        "N->lu_child[!side] is not sib");
  check(centree_lu_parent(best.sib) == build.node, "sib still points at inner");
  check(centree_lu_parent(best.star) == build.node,
        "star still points at outer");
  if (best.up)
    check(best.up->lu_child[CENTREE_LU_LEFT] == build.node ||
              best.up->lu_child[CENTREE_LU_RIGHT] == build.node,
          "D does not have N as a history child");
  check(centree_lu_parent(best.leaf) == best.inner,
        "the leaf's own lu_parent was touched");
  check(atomic_load(&best.leaf->value.slab->full) == 1,
        "the leaf was not frozen");

  /* N holds the merge of all three, the leaf winning. */
  srcs[0] = best.leaf;
  srcs[1] = best.inner;
  srcs[2] = best.outer;
  build_expected(srcs, 3);
  collect_got(build.slab);
  verify_merged(build.slab, "after link");
  printf("  %-34s slab %lu, %zu entries (cold<=%zu)\n", "history link", 
         build.slab->seq, build.count, best.cold_bound);

  verify_reads("reads across the history link");

  /* A writer whose key routes to the frozen leaf must park, not spin. */
  struct req blocked = {0};

  blocked.item = make_item(blocked_key, blocked_value);
  blocked.cb.cb = write_done;
  blocked.cb.item = (char *)blocked.item;
  blocked.cb.fsst_idx = -1;
  atomic_store(&blocked.done, 0);
  kv_upsert_async(&blocked.cb);
  usleep(100000);
  check(!atomic_load(&blocked.done), "a writer got into the frozen leaf (key %lu)",
        blocked_key);

  prune_splice_routing(&best, &build);
  tnt_maintenance_unlock();

  for (int i = 0; i < 5000 && !atomic_load(&blocked.done); i++) usleep(1000);
  check(atomic_load(&blocked.done),
        "the parked writer never completed after the splice");
  free(blocked.item);

  check(atomic_load(&best.leaf->removed) && atomic_load(&best.inner->removed) &&
            atomic_load(&best.outer->removed),
        "the triple was not retired");
  check(centree_validate_locked(tnt_centree()),
        "the routing tree does not validate after the splice");
  check(tnt_get_node_count() == nb_before - 2, "node_count is %lu, expected %zu",
        tnt_get_node_count(), nb_before - 2);

  /* The old sequence, with the triple replaced by N in its place. */
  for (size_t i = 0; i < nb_before; i++) {
    if (before[i] == best.leaf || before[i] == best.inner ||
        before[i] == best.outer) {
      if (nb_want == 0 || want[nb_want - 1] != build.node)
        want[nb_want++] = build.node;
      continue;
    }
    want[nb_want++] = before[i];
  }
  collect_routing(routing_root(), after, cap2, &nb_after);
  check(nb_after == nb_want, "in-order has %zu nodes, expected %zu", nb_after,
        nb_want);
  if (nb_after == nb_want)
    for (size_t i = 0; i < nb_after; i++)
      check(after[i] == want[i], "in-order position %zu changed", i);

  validate("after one prune");
  verify_reads("reads across the splice");
  printf("  %-34s slab %lu took over from %lu/%lu/%lu\n", "routing splice",
         build.slab->seq, best.inner->value.slab->seq,
         best.leaf->value.slab->seq, best.outer->value.slab->seq);
  /* Retire too, so the file accounting stays exact. */
  prune_retire(&best);
  free(before);
  free(want);
  free(after);
  /* N is live now: the build must not be discarded. */
}

static int prune_one(void);

/* The two config knobs must be able to hold the pruner off. */
static void check_prune_config(void) {
  size_t nodes = tnt_get_node_count();

  check(prune_count_candidates() > 0, "nothing is prunable to begin with");

  cfg.prune_margin = 1u << 30;
  check(prune_count_candidates() == 0, "prune_margin did not reject anything");
  check(tnt_prune_once() == TNT_PRUNE_NOOP, "a prune slipped past the margin");
  cfg.prune_margin = 0;

  cfg.prune_min_age = 1u << 30;
  check(prune_count_candidates() == 0, "prune_min_age did not reject anything");
  check(tnt_prune_once() == TNT_PRUNE_NOOP, "a prune slipped past the min age");
  cfg.prune_min_age = 0;

  check(tnt_get_node_count() == nodes, "a rejected prune changed the tree");
  check(prune_count_candidates() > 0, "the knobs did not come back");
  printf("  %-34s margin and min-age both hold it off\n", "prune config");
}

/* Prune until nothing is prunable any more. */
static void check_prune_all(void) {
  struct prune_stale before, after;
  size_t done = 0;

  /* The stale-slot estimate that the automatic trigger acts on. */
  prune_stale_measure(&before);
  check(before.nodes == tnt_get_node_count(),
        "stale estimate saw %zu nodes, node_count %lu", before.nodes,
        tnt_get_node_count());
  check(before.stale > 0, "no stale slot before the sweep although the "
        "overwrite passes invalidated entries");

  for (;;) {
    int r = prune_one();

    if (r == 0) break;
    if (r < 0) {
      /* Nothing changed, and the scan would hand back the same candidate. */
      printf("  %-34s a candidate was dropped after %zu prunes\n",
             "prune sweep", done);
      break;
    }
    if (++done > 500) break;
  }
  check(done > 0, "the sweep pruned nothing");
  check(centree_validate_locked(tnt_centree()),
        "the routing tree does not validate after the sweep");
  validate("after the prune sweep");
  verify_reads("reads across the sweep");
  printf("  %-34s %zu prunes, %lu nodes left\n", "prune sweep", done,
         tnt_get_node_count());

  /* Pruning removes stale slots and nothing else: valid stays, stale drops. */
  prune_stale_measure(&after);
  check(after.stale < before.stale,
        "stale slots did not drop across the sweep (%zu -> %zu)", before.stale,
        after.stale);
  check(after.valid <= before.valid,
        "valid entries grew across the sweep (%zu -> %zu)", before.valid,
        after.valid);
  printf("  %-34s %zu/%zu stale (%.1f%%) -> %zu/%zu (%.1f%%)\n",
         "stale estimate", before.stale, before.reserved,
         100.0 * prune_stale_ratio(&before), after.stale, after.reserved,
         100.0 * prune_stale_ratio(&after));
}

/* The pruned tree must still take writes and serve them. */
static void check_writes_after_prune(void) {
  size_t bad = 0;

  run_ops(0, nb_keys, 10, 0, 9, 0, 1);
  for (uint64_t k = 0; k < nb_keys; k++) {
    if (k % 10 == 0) continue;
    read_back_quiet(k);
    if (rb_found && rb_value == k * 7 + 9) continue;
    if (bad < 5)
      check(false, "key %lu reads found=%d value=%lu, want %lu", k, rb_found,
            rb_value, k * 7 + 9);
    bad++;
  }
  check(bad == 0, "%zu keys wrong after writing into the pruned tree", bad);
  if (!bad)
    printf("  %-34s %lu keys rewritten and read back\n", "writes after prune",
           nb_keys - nb_keys / 10);
}

/* -------------------------------------------------- retire and release */

static int slab_path(struct slab *s, char *out, size_t len) {
  char proc[64];
  int n;

  if (s->fd < 0) return 0;
  snprintf(proc, sizeof(proc), "/proc/self/fd/%d", s->fd);
  n = readlink(proc, out, len - 1);
  if (n <= 0) return 0;
  out[n] = 0;
  return 1;
}

static size_t count_slab_files(void) {
  struct dirent *entry;
  DIR *dir = opendir("/scratch0/kvell");
  size_t n = 0;

  if (!dir) return 0;
  while ((entry = readdir(dir)) != NULL)
    if (!strncmp(entry->d_name, "slab-", 5)) n++;
  closedir(dir);
  return n;
}

/*
 * A full prune including retirement, with one of the retired slabs pinned as
 * if a read were still in flight: its file must survive until the reference
 * goes away, and disappear right afterwards.
 */
static void check_retire(void) {
  size_t cap = tnt_get_node_count() + 8;
  centree_node *order = calloc(cap, sizeof(*order));
  struct prune_candidate best;
  struct prune_build build;
  char paths[3][512];
  int has_path[3];
  struct slab *slabs[3];
  size_t nb_order = 0, files_before;
  bool have = false;
  int error;

  collect_routing(routing_root(), order, cap, &nb_order);
  for (size_t i = 0; i < nb_order; i += 2) {
    struct prune_candidate c;

    if (!prune_select(order[i], &c)) continue;
    best = c;
    have = true;
    break;
  }
  free(order);
  if (!have) {
    check(false, "no candidate left to retire");
    return;
  }

  files_before = count_slab_files();
  slabs[0] = best.leaf->value.slab;
  slabs[1] = best.inner->value.slab;
  slabs[2] = best.outer->value.slab;
  for (int i = 0; i < 3; i++)
    has_path[i] = slab_path(slabs[i], paths[i], sizeof(paths[i]));
  check(has_path[0] && has_path[1] && has_path[2],
        "could not resolve the triple's file names");

  /* Pretend a read is still in flight on the inner slab. */
  __sync_fetch_and_add(&slabs[1]->read_ref, 1);

  tnt_maintenance_lock();
  error = prune_freeze_and_link(&best, &build);
  if (!error) {
    prune_splice_routing(&best, &build);
    prune_retire(&best);
  }
  tnt_maintenance_unlock();
  check(error == 0, "the prune failed with %d", error);
  if (error) {
    __sync_fetch_and_sub(&slabs[1]->read_ref, 1);
    return;
  }

  for (int i = 0; i < 3; i++) {
    check(slabs[i]->subtree == NULL, "slab %lu kept its local index",
          slabs[i]->seq);
    check(slabs[i]->min == (uint64_t)-1 && slabs[i]->max == 0,
          "slab %lu kept its range", slabs[i]->seq);
  }
  check(slabs[0]->fd == -1 && slabs[2]->fd == -1,
        "an idle retired slab kept its file open");
  check(access(paths[0], F_OK) != 0 && access(paths[2], F_OK) != 0,
        "an idle retired slab's file is still there");
  /* The pinned one must still be intact. */
  check(slabs[1]->fd != -1, "the pinned slab was closed");
  check(access(paths[1], F_OK) == 0, "the pinned slab's file was unlinked");

  __sync_fetch_and_sub(&slabs[1]->read_ref, 1);
  slab_release_if_idle(slabs[1]);
  check(slabs[1]->fd == -1, "the last reference did not close the file");
  check(access(paths[1], F_OK) != 0, "the last reference did not unlink it");

  /* Three files gone, one added. */
  check(count_slab_files() == files_before - 2,
        "%zu slab files, expected %zu", count_slab_files(), files_before - 2);
  check(access(paths[0], F_OK) != 0, "leaf file reappeared");
  validate("after retire");
  verify_reads("reads across the retire");
  printf("  %-34s %zu files -> %zu, one pinned until its reader left\n",
         "retire and release", files_before, count_slab_files());
}

/* ------------------------------------------- prunes under live traffic */

/*
 * Readers and writers hammering the whole key range while the pruner works.
 * The writers rewrite each key with the value it already has, so the expected
 * state never changes and any read that disagrees is a real fault: a miss on a
 * live key, a resurrected tombstone, or a stale value.
 */
static _Atomic int stress_stop;
static _Atomic size_t stress_reads, stress_writes, stress_bad, stress_corrupt;

static uint64_t stress_value(uint64_t k) { return k * 7 + 9; }
static int stress_deleted(uint64_t k) { return k % 10 == 0; }

struct stress_req {
  struct slab_callback cb;
  unsigned char *item;
  _Atomic int done;
  int found;
  uint64_t key;
  uint64_t got_key;
  uint64_t value;
};

static void stress_read_done(struct slab_callback *cb, void *item) {
  struct stress_req *r = (struct stress_req *)cb;
  struct item_metadata *meta = item;

  r->found = item != NULL;
  if (item) {
    r->got_key = *(uint64_t *)((unsigned char *)item + sizeof(*meta));
    r->value =
        *(uint64_t *)((unsigned char *)item + sizeof(*meta) + sizeof(uint64_t));
  }
  atomic_store(&r->done, 1);
}

static void stress_write_done(struct slab_callback *cb, void *item) {
  (void)item;
  atomic_store(&((struct stress_req *)cb)->done, 1);
}

static void *stress_reader(void *arg) {
  unsigned seed = (unsigned)(uintptr_t)arg * 7919 + 13;

  while (!atomic_load(&stress_stop)) {
    struct stress_req r = {0};
    uint64_t k = rand_r(&seed) % nb_keys;

    r.item = make_item(k, 0);
    r.key = k;
    r.cb.cb = stress_read_done;
    r.cb.item = (char *)r.item;
    r.cb.fsst_idx = -1;
    atomic_store(&r.done, 0);
    kv_read_async(&r.cb);
    while (!atomic_load(&r.done)) NOP10();
    atomic_fetch_add(&stress_reads, 1);
    if (r.found && r.got_key != k) {
      /*
       * The index pointed at a slot holding somebody else's record. That is
       * on-disk corruption, and the only known source is reinsertion writing
       * at the source's slot index (see PRUNING_NOTES.md). Counted apart from
       * the model check, which is about what pruning could get wrong.
       */
      size_t slot = atomic_fetch_add(&stress_corrupt, 1);

      if (slot < 8)
        printf("    slot corruption: read key %lu, got a record for key %lu\n",
               k, r.got_key);
    } else if (stress_deleted(k) ? r.found
                                 : (!r.found || r.value != stress_value(k))) {
      size_t slot = atomic_fetch_add(&stress_bad, 1);

      if (slot < 8)
        printf("    read anomaly: key %lu found=%d value=%lu want %s%lu\n", k,
               r.found, r.value, stress_deleted(k) ? "miss/" : "",
               stress_value(k));
    }
    free(r.item);
  }
  return NULL;
}

static void *stress_writer(void *arg) {
  unsigned seed = (unsigned)(uintptr_t)arg * 104729 + 7;

  while (!atomic_load(&stress_stop)) {
    struct stress_req r = {0};
    uint64_t k = rand_r(&seed) % nb_keys;

    if (stress_deleted(k)) k++;
    r.item = make_item(k, stress_value(k));
    r.cb.cb = stress_write_done;
    r.cb.item = (char *)r.item;
    r.cb.fsst_idx = -1;
    atomic_store(&r.done, 0);
    kv_upsert_async(&r.cb);
    while (!atomic_load(&r.done)) NOP10();
    atomic_fetch_add(&stress_writes, 1);
    free(r.item);
  }
  return NULL;
}

/*
 * Hand a live slab to the reinsertion worker, so its pass overlaps the
 * pruner: it may end up scanning a slab that gets retired underneath it,
 * which it must notice instead of reading a closed or reused file.
 */
static void queue_for_reinsertion(unsigned *seed) {
  size_t cap = tnt_get_node_count() + 8;
  centree_node *order = calloc(cap, sizeof(*order));
  size_t nb_order = 0;
  struct slab *s;

  collect_routing(routing_root(), order, cap, &nb_order);
  if (nb_order == 0) {
    free(order);
    return;
  }
  s = order[rand_r(seed) % nb_order]->value.slab;
  free(order);
  if (!s->hot_bits) return;
  for (size_t p = 0; p < slab_data_size(s) / PAGE_SIZE; p++)
    mark_page_hot(s, p);
  if (!atomic_load(&s->queued)) {
    int expected = 0;

    if (atomic_compare_exchange_strong(&s->queued, &expected, 1))
      bgq_enqueue(GC, s);
  }
}

/* The production entry point: 1 pruned, 0 nothing to do, -1 dropped. */
static size_t shy_seen;
static void count_shy_cb(uint64_t key, uint32_t slot, void *data) {
  (void)key; (void)data;
  if (sidx_is_shy(slot) && !sidx_is_invalid(slot)) shy_seen++;
}

static size_t count_shy_entries(void) {
  size_t cap = tnt_get_node_count() + 8;
  centree_node *order = calloc(cap, sizeof(*order));
  size_t nb_order = 0;

  shy_seen = 0;
  collect_routing(routing_root(), order, cap, &nb_order);
  for (size_t i = 0; i < nb_order; i++) {
    struct slab *s = order[i]->value.slab;

    R_LOCK(&s->tree_lock);
    if (s->subtree) subtree_forall_entries(s->subtree, count_shy_cb, NULL);
    R_UNLOCK(&s->tree_lock);
  }
  free(order);
  return shy_seen;
}

/* True when no slab in the routing tree holds any reference. */
static bool refs_quiescent(void) {
  size_t cap = tnt_get_node_count() + 8;
  centree_node *order = calloc(cap, sizeof(*order));
  size_t nb_order = 0;
  bool quiet = true;

  collect_routing(routing_root(), order, cap, &nb_order);
  for (size_t i = 0; i < nb_order && i < cap && quiet; i++) {
    struct slab *s = order[i]->value.slab;

    if (__sync_fetch_and_or(&s->update_ref, 0) != 0 ||
        __sync_fetch_and_or(&s->read_ref, 0) != 0)
      quiet = false;
  }
  free(order);
  return quiet;
}

static int prune_one(void) {
  int status = tnt_prune_once();

  if (status == TNT_PRUNE_DONE) return 1;
  if (status == TNT_PRUNE_NOOP) return 0;
  check(status == -EAGAIN || status == -EBUSY || status == -ENOSPC,
        "tnt_prune_once() failed with %d", status);
  return -1;
}

static void check_concurrent_prune(void) {
  pthread_t readers[4], writers[2];
  size_t prunes = 0, rejects = 0, idle = 0;

  atomic_store(&stress_stop, 0);
  for (size_t i = 0; i < 4; i++)
    pthread_create(&readers[i], NULL, stress_reader, (void *)i);
  for (size_t i = 0; i < 2; i++)
    pthread_create(&writers[i], NULL, stress_writer, (void *)i);

  unsigned seed = 20250902;

  for (int round = 0; round < 400; round++) {
    int r = getenv("PRUNE_STRESS_NOPRUNE") ? 0 : prune_one();

    if (cfg.with_reins) queue_for_reinsertion(&seed);

    if (r > 0) prunes++;
    else if (r < 0) rejects++;
    else {
      idle++;
      usleep(5000);
    }
    /* Rebalancing has to interleave safely with pruning too. */
    if (round % 50 == 49 && !getenv("PRUNE_STRESS_NOREBAL"))
      check(tnt_rebalancing() >= 0, "rebalancing failed");
  }

  atomic_store(&stress_stop, 1);
  for (size_t i = 0; i < 4; i++) pthread_join(readers[i], NULL);
  for (size_t i = 0; i < 2; i++) pthread_join(writers[i], NULL);

  /*
   * The clients are done, but the reinsertion worker is not a client: it may
   * still be inside a batch, pinning its source and with copy-forwards in
   * flight to a leaf. Give those references a bounded time to drain. What is
   * still held after that is held by nothing legitimate, and validate()
   * reports it as a leak.
   */
  {
    int waited_ms = 0;

    while (waited_ms < 5000 && !refs_quiescent()) {
      usleep(10000);
      waited_ms += 10;
    }
    if (waited_ms)
      printf("  %-34s %d ms for background references to drain\n",
             "quiescence", waited_ms);
  }

  /*
   * Without the reinsertion worker the model is exact and any disagreement is
   * a fault. With it, two pre-existing defects can put an older version back
   * in front (a copy-forward racing a client write, and fsst.c writing at the
   * source's slot index); AGENTS.md leaves both unfixed, so they are counted
   * and reported rather than failed. See PRUNING_NOTES.md.
   */
  /*
   * With shy reinsertion the model is exact in both modes: a copy-forward
   * never shadows or drops a client write, and never lands at another slot.
   */
  check(atomic_load(&stress_bad) == 0, "%zu reads disagreed with the model",
        (size_t)atomic_load(&stress_bad));
  check(atomic_load(&stress_corrupt) == 0, "%zu reads hit a corrupted slot",
        (size_t)atomic_load(&stress_corrupt));
  check(prune_bad_slot_count() == 0,
        "%lu slots held a record the index did not name",
        prune_bad_slot_count());
  if (atomic_load(&stress_bad))
    printf("    NOTE %zu reads returned an older version (pre-existing "
           "reinsertion race, see PRUNING_NOTES.md)\n",
           (size_t)atomic_load(&stress_bad));
  if (prune_bad_slot_count() || atomic_load(&stress_corrupt))
    printf("    NOTE %lu slots held a record the index did not name, "
           "%zu reads hit one (pre-existing: reinsertion writes at the "
           "source slot, see PRUNING_NOTES.md)\n",
           prune_bad_slot_count(), (size_t)atomic_load(&stress_corrupt));
  /*
   * Whether the pruner finds work during these 400 rounds depends on what the
   * earlier sweep left and on what the writers happen to drain; the sweep
   * phase already asserts that pruning works. What this phase asserts is
   * consistency under concurrent pruning.
   */
  if (prunes == 0 && !getenv("PRUNE_STRESS_NOPRUNE"))
    printf("    NOTE no candidate came up during the stress phase this run\n");
  check(centree_validate_locked(tnt_centree()),
        "the routing tree does not validate after the stress run");
  validate("after concurrent prunes");
  printf("  %-34s %zu prunes (%zu rejected, %zu idle), %zu reads, %zu writes\n",
         "prunes under load", prunes, rejects, idle,
         (size_t)atomic_load(&stress_reads), (size_t)atomic_load(&stress_writes));

  /* Proof that reinsertion did move records, not just abort: shy entries exist. */
  if (cfg.with_reins) {
    size_t shy = count_shy_entries();

    check(shy > 0, "reinsertion published no copy at all during the run");
    printf("  %-34s %zu shy entries in the index\n", "reinsertion published", shy);
  }
}

/*
 * The state a completed run leaves behind: the deleted tenth is gone, every
 * other key below nb_keys carries the value the last write phase gave it, and
 * the upper range still carries its load value. Used by the "verify" mode to
 * re-open an existing database, which is the only recovery check this suite
 * makes.
 */
static void verify_model(void) {
  uint64_t hi = nb_keys + nb_keys / 2;
  size_t bad = 0;

  printf("  recovered %lu index entries\n", get_database_size());
  for (uint64_t k = 0; k < hi; k++) {
    int want_found = 1;
    uint64_t want = 0;

    if (k >= nb_keys)
      want = k * 7 + 1;
    else if (k % 10 == 0)
      want_found = 0;
    else
      want = k * 7 + 9;

    read_back_quiet(k);
    if (rb_found == want_found && (!want_found || rb_value == want)) continue;
    if (bad < 8)
      check(false, "key %lu: found %d want %d, value %lu want %lu", k, rb_found,
            want_found, rb_value, want);
    bad++;
  }
  check(bad == 0, "%zu of %lu keys wrong after recovery", bad, hi);
  if (!bad) printf("  %-34s %lu keys\n", "state after recovery", hi);
}

/*
 * The state after the load and overwrite phases, before any prune: the
 * deleted tenth is gone, the rest of the lower range carries version 3, the
 * upper range its load value. The crash modes stop after these phases, so a
 * recovered database has to show exactly this.
 */
static void verify_pre_prune_model(void) {
  uint64_t hi = nb_keys + nb_keys / 2;
  size_t bad = 0;

  printf("  recovered %lu index entries\n", get_database_size());
  for (uint64_t k = 0; k < hi; k++) {
    int want_found = 1;
    uint64_t want = 0;

    if (k >= nb_keys)
      want = k * 7 + 1;
    else if (k % 10 == 0)
      want_found = 0;
    else
      want = k * 7 + 3;

    read_back_quiet(k);
    if (rb_found == want_found && (!want_found || rb_value == want)) continue;
    if (bad < 8)
      check(false, "key %lu: found %d want %d, value %lu want %lu", k, rb_found,
            want_found, rb_value, want);
    bad++;
  }
  check(bad == 0, "%zu of %lu keys wrong after recovering from a crash", bad,
        hi);
  check(count_slab_files() == tnt_get_node_count(),
        "%zu slab files for %lu nodes: recovery left garbage", count_slab_files(),
        tnt_get_node_count());
  validate("after crash recovery");
  if (!bad) printf("  %-34s %lu keys\n", "state after crash recovery", hi);
}

/*
 * After a crash in the middle of the initial load nothing is known about
 * which writes were acknowledged, so only consistency is checked: recovery
 * succeeds, nothing unreachable is left behind, both trees agree, and every
 * readable key carries a value some version of the workload wrote for it.
 */
static void verify_consistent_only(void) {
  uint64_t hi = nb_keys + nb_keys / 2;
  size_t bad = 0, found = 0;

  printf("  recovered %lu index entries\n", get_database_size());
  for (uint64_t k = 0; k < hi; k++) {
    read_back_quiet(k);
    if (!rb_found) continue;
    found++;
    if (rb_value == k * 7 + 1 || rb_value == k * 7 + 2 || rb_value == k * 7 + 3)
      continue;
    if (bad < 8)
      check(false, "key %lu reads %lu, no version of the workload wrote that", k,
            rb_value);
    bad++;
  }
  check(bad == 0, "%zu keys hold values nobody wrote", bad);
  check(found > 0, "nothing at all survived the crash");
  check(count_slab_files() == tnt_get_node_count(),
        "%zu slab files for %lu nodes: recovery left garbage", count_slab_files(),
        tnt_get_node_count());
  validate("after crash recovery");
  printf("  %-34s %zu keys readable, all consistent\n",
         "state after crash recovery", found);
}

/* ------------------------------------------------ a hole in a live leaf */

/*
 * A crash between a slot's reservation and its page write leaves an empty
 * slot in the middle of a leaf. hole-punch loads the even keys, zeroes the
 * second occupied slot of some live leaf and records what was there;
 * hole-verify recovers, appends a fresh odd key that routes to that leaf, and
 * checks that every other record of the leaf survived -- both the ones after
 * the hole (recovery must not stop at it) and the last one (the append must
 * land past the highest occupied slot, not on top of it).
 */
#define HOLE_INFO "/scratch0/kvell/holetest"

static void hole_punch(void) {
  centree_node leaf = NULL;
  struct slab *s;
  size_t ipp;
  char *page;
  uint64_t lost_key, probe_key;
  FILE *info;

  run_ops(0, nb_keys, 2, 0, 1, 0, 0); /* even keys only, version 1 */

  /* A live leaf with at least three records. */
  for (uint64_t k = 0; k < nb_keys && !leaf; k += 2) {
    centree_node n = leaf_for_key(k);

    if (n && n->value.slab->nb_items >= 3) leaf = n;
  }
  if (!leaf) {
    check(false, "no leaf with three records");
    return;
  }
  s = leaf->value.slab;
  ipp = PAGE_SIZE / s->item_size;
  page = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  check(pread(s->fd, page, PAGE_SIZE, 0) == (ssize_t)PAGE_SIZE, "pread failed");
  lost_key = *(uint64_t *)(page + 1 * s->item_size + sizeof(struct item_metadata));
  memset(page + 1 * s->item_size, 0, s->item_size); /* slot 1 becomes a hole */
  check(pwrite(s->fd, page, PAGE_SIZE, 0) == (ssize_t)PAGE_SIZE, "pwrite failed");
  fsync(s->fd);
  (void)ipp;

  /* An odd key inside the leaf's range that is not stored anywhere. */
  probe_key = s->min + 1;
  while (probe_key % 2 == 0 || probe_key > s->max) probe_key++;
  check(probe_key <= s->max, "no odd key fits the leaf's range");

  info = fopen(HOLE_INFO, "w");
  fprintf(info, "%lu %lu %lu %lu\n", lost_key, probe_key, s->seq, s->nb_items);
  fclose(info);
  printf("  %-34s slab %lu: lost key %lu, probe key %lu\n", "hole punched",
         s->seq, lost_key, probe_key);
  free(page);
}

static void hole_verify(void) {
  uint64_t lost_key, probe_key, seq, nb_before;
  FILE *info = fopen(HOLE_INFO, "r");
  size_t bad = 0;

  if (!info || fscanf(info, "%lu %lu %lu %lu", &lost_key, &probe_key, &seq,
                      &nb_before) != 4) {
    check(false, "no hole information");
    return;
  }
  fclose(info);
  printf("  recovered %lu index entries\n", get_database_size());

  /* The append must go past the highest occupied slot. */
  run_ops(probe_key, probe_key + 1, 0, 0, 5, 0, 0);

  for (uint64_t k = 0; k < nb_keys; k += 2) {
    read_back_quiet(k);
    if (k == lost_key) {
      if (rb_found) check(false, "the hole's key %lu came back", k);
      continue;
    }
    if (rb_found && rb_value == k * 7 + 1) continue;
    if (bad < 8)
      check(false, "key %lu: found %d value %lu, want %lu", k, rb_found, rb_value,
            k * 7 + 1);
    bad++;
  }
  read_back(probe_key, probe_key * 7 + 5);
  check(bad == 0, "%zu records lost around the hole", bad);
  validate("after recovering a holed leaf");
  if (!bad)
    printf("  %-34s hole at slab %lu skipped, %lu records kept\n",
           "state after recovery", seq, nb_before - 1);
}

/* ------------------------------------------ the automatic trigger (-C) */

/*
 * The same load and overwrite passes as the main run, then the restructuring
 * worker is started as -C would start it, with a short period and a threshold
 * the sweep is known to reach. Nobody calls tnt_prune_once() here: the worker
 * has to notice the stale ratio on its own timer and bring it under the
 * threshold, and every key must still read as before.
 */
static void check_auto_trigger(void) {
  struct prune_stale m;
  int waited_ms = 0;
  size_t nodes_before;

  run_upserts(0, nb_keys);
  check(tnt_rebalancing() >= 0, "tnt_rebalancing() failed");
  run_upserts(nb_keys, nb_keys + nb_keys / 2);
  run_ops(0, nb_keys, 10, 0, 2, 0, 1);
  run_ops(0, nb_keys, 10, 0, 0, 1, 0);
  run_ops(0, nb_keys, 10, 0, 3, 0, 1);
  validate("before the automatic trigger");
  snapshot_reads(nb_keys + nb_keys / 2);
  nodes_before = tnt_get_node_count();

  cfg.with_prune = 1;
  cfg.prune_auto = 1;
  cfg.prune_stale_ratio = 0.2;
  cfg.prune_period_ms = 50;
  prune_stale_measure(&m);
  check(prune_stale_ratio(&m) >= cfg.prune_stale_ratio,
        "the load left only %.1f%% stale, below the %.0f%% threshold",
        100.0 * prune_stale_ratio(&m), 100.0 * cfg.prune_stale_ratio);
  printf("  %-34s %zu/%zu stale (%.1f%%), threshold %.0f%%\n",
         "before the automatic trigger", m.stale, m.reserved,
         100.0 * prune_stale_ratio(&m), 100.0 * cfg.prune_stale_ratio);

  check(restructuring_worker_init() == 0, "restructuring worker did not start");
  while (waited_ms < 30000) {
    prune_stale_measure(&m);
    if (prune_stale_ratio(&m) < cfg.prune_stale_ratio) break;
    usleep(100000);
    waited_ms += 100;
  }
  /* Park the worker: no more automatic prunes while the checks run. */
  cfg.prune_auto = 0;
  cfg.with_prune = 0;
  tnt_maintenance_lock(); /* waits for a prune in flight */
  tnt_maintenance_unlock();
  prune_stale_measure(&m);

  check(prune_stale_ratio(&m) < cfg.prune_stale_ratio,
        "after %d ms the stale ratio is still %.1f%%", waited_ms,
        100.0 * prune_stale_ratio(&m));
  check(tnt_get_node_count() < nodes_before,
        "the worker pruned nothing (%lu nodes before and after)", nodes_before);
  check(centree_validate_locked(tnt_centree()),
        "the routing tree does not validate after automatic pruning");
  validate("after automatic pruning");
  verify_reads("reads after automatic pruning");
  check(count_slab_files() == tnt_get_node_count(),
        "%zu slab files for %lu nodes after automatic pruning",
        count_slab_files(), tnt_get_node_count());
  printf("  %-34s %zu/%zu stale (%.1f%%) after %d ms, %lu -> %lu nodes\n",
         "automatic pruning", m.stale, m.reserved,
         100.0 * prune_stale_ratio(&m), waited_ms, nodes_before,
         tnt_get_node_count());
}

int main(int argc, char **argv) {
  int verify_only = argc > 1 && !strcmp(argv[1], "verify");
  int hole_punch_mode = argc > 1 && !strcmp(argv[1], "hole-punch");
  int hole_verify_mode = argc > 1 && !strcmp(argv[1], "hole-verify");
  int verify_crash = argc > 1 && !strcmp(argv[1], "verify-crash");
  int verify_consistent = argc > 1 && !strcmp(argv[1], "verify-consistent");
  int crash_split = argc > 1 && !strcmp(argv[1], "crash-split");
  int crash_prune = argc > 1 && !strncmp(argv[1], "crash-prune", 11);
  int auto_mode = argc > 1 && !strcmp(argv[1], "auto");
  int mode_arg = verify_only || verify_crash || verify_consistent ||
                 crash_split || crash_prune || hole_punch_mode ||
                 hole_verify_mode || auto_mode;

  if (argc > 1 && !mode_arg) nb_keys = strtoull(argv[1], NULL, 0);
  if (argc > 2) nb_keys = strtoull(argv[2], NULL, 0);

  init_default_config(&cfg);
  cfg.kv_size = TEST_KV_SIZE;
  cfg.max_file_size = TEST_MAX_FILE_SIZE;
  cfg.page_cache_size = PAGE_SIZE * 8192; /* 32 MB */
  /* PRUNE_REINS=1 runs the background reinsertion worker alongside. */
  cfg.with_reins = getenv("PRUNE_REINS") ? 1 : 0;
  cfg.with_rebal = 0;
  cfg.nb_items_in_db = nb_keys;

  printf("== history links (%lu keys, reins=%d) ==\n", nb_keys,
         cfg.with_reins);
  slab_workers_init(1, 4, 2);
  if (cfg.with_reins) fsst_worker_init();

  if (verify_only || verify_crash || verify_consistent || hole_punch_mode ||
      hole_verify_mode) {
    if (verify_only)
      verify_model();
    else if (verify_crash)
      verify_pre_prune_model();
    else if (verify_consistent)
      verify_consistent_only();
    else if (hole_punch_mode)
      hole_punch();
    else
      hole_verify();
    if (failures) {
      printf("== %lu failures ==\n", failures);
      return 1;
    }
    printf("== ok ==\n");
    return 0;
  }

  if (auto_mode) {
    check_auto_trigger();
    if (failures) {
      printf("== %lu failures ==\n", failures);
      return 1;
    }
    printf("== ok ==\n");
    return 0;
  }

  /* crash-split: die inside the first split, i.e. during the load below. */
  if (crash_split) slab_set_crash_point(CRASH_SPLIT_BEFORE_COMMIT);

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

  /*
   * crash-prune-<n>: everything above is durable and acknowledged; now die at
   * crash point n inside the first prune. The verify-crash mode checks that
   * the recovered database shows exactly the pre-prune state.
   */
  if (crash_prune) {
    int point = atoi(argv[1] + 12); /* after "crash-prune-" */

    check(prune_count_candidates() > 0, "nothing to prune for the crash test");
    slab_set_crash_point((enum slab_crash_point)point);
    printf("  crashing at point %d inside a prune\n", point);
    tnt_prune_once();
    check(false, "the crash point %d was never reached", point);
    return failures ? 1 : 0;
  }
  check_selection("candidates after overwrites");
  check_cold_builds();
  validate("after cold builds");

  /* An early key sits in an internal slab, a late one in its leaf. */
  check_read_restart(0);
  check_read_restart(nb_keys + nb_keys / 2 - 1);

  /*
   * Not a multiple of 10: the deleted tenth must stay deleted for the stress
   * phase's model, and this key gets rewritten by check_writes_after_prune().
   */
  check_blocked_writer(nb_keys / 2 + 1);
  validate("after blocked-writer restart");

  check_prune_config();
  check_prune_link();
  check_retire();
  check_prune_all();
  check(count_slab_files() == tnt_get_node_count(),
        "%zu slab files for %lu nodes", count_slab_files(),
        tnt_get_node_count());
  check_writes_after_prune();
  check_concurrent_prune();
  check(count_slab_files() == tnt_get_node_count(),
        "%zu slab files for %lu nodes after the stress run", count_slab_files(),
        tnt_get_node_count());

  /* The rule has two orientations and a history-root case; cover all three. */
  check(cov_candidates > 0, "no prunable triple was ever selected");
  check(cov_side[0] > 0 && cov_side[1] > 0,
        "only one orientation was covered (side0 %zu, side1 %zu)", cov_side[0],
        cov_side[1]);
  /*
   * With the reinsertion worker on, its copy-forwards keep appending into the
   * leaves near the root and can keep the root triple just over the fit test
   * (or split its leaf) for a whole run, so the plain run is the one that has
   * to prove the history-root case. The reinsertion run only reports it.
   */
  if (!cfg.with_reins)
    check(cov_history_root > 0, "the D == NULL case was never seen");
  else if (cov_history_root == 0)
    printf("    NOTE the D == NULL case did not come up in this run\n");

  if (failures) {
    printf("== %lu failures ==\n", failures);
    return 1;
  }
  printf("== ok ==\n");
  return 0;
}
