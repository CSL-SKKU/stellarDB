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

static void run_upserts(uint64_t lo, uint64_t hi) {
  uint64_t n = hi - lo;
  struct req *reqs = calloc(n, sizeof(*reqs));

  for (uint64_t i = 0; i < n; i++) {
    struct req *r = &reqs[i];

    r->item = make_item(lo + i, (lo + i) * 7 + 1);
    r->cb.cb = write_done;
    r->cb.item = (char *)r->item;
    r->cb.fsst_idx = -1;
    atomic_store(&r->done, 0);
    kv_upsert_async(&r->cb);
  }
  for (uint64_t i = 0; i < n; i++)
    while (!atomic_load(&reqs[i].done)) NOP10();
  for (uint64_t i = 0; i < n; i++) free(reqs[i].item);
  free(reqs);
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

  check(tnt_rebalancing() >= 0, "tnt_rebalancing() failed");
  validate("after rebalance");

  /* Writes that land in the rebalanced tree must keep history consistent. */
  run_upserts(nb_keys, nb_keys + nb_keys / 2);
  validate("after post-rebalance writes");

  /* An early key sits in an internal slab, a late one in its leaf. */
  check_read_restart(0);
  check_read_restart(nb_keys + nb_keys / 2 - 1);

  if (failures) {
    printf("== %lu failures ==\n", failures);
    return 1;
  }
  printf("== ok ==\n");
  return 0;
}
