#include "headers.h"
#include <errno.h>

int print, load, rc_thr = 1;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define NODES 63
#define KEYS 24
static uint64_t key_at(unsigned k) {
  return k == 0 ? 0 : k == 1 ? UINT64_MAX : UINT64_C(0x9e3779b97f4a7c15) * k;
}
static unsigned rng = 83179;
static unsigned draw(void) { rng ^= rng << 13; rng ^= rng >> 17; return rng ^= rng << 5; }
static size_t position;
static void preorder(struct subtree_idle_node *nodes, int depth, size_t parent) {
  size_t here = position++;
  nodes[here].parent = parent;
  if (depth) { preorder(nodes, depth - 1, here); preorder(nodes, depth - 1, here); }
}
static void tick(void *context) { ++*(size_t *)context; }

static void check_random_history(void) {
  for (unsigned trial = 0; trial < 512; trial++) {
    struct subtree_idle_node nodes[NODES];
    size_t valid[NODES] = {0};
    unsigned char present[NODES][KEYS] = {{0}}, marked[NODES][KEYS] = {{0}};
    unsigned char tomb[NODES][KEYS] = {{0}}, shy[NODES][KEYS] = {{0}};
    uint64_t expected_new = 0, observed = 0;
    size_t ticks = 0;
    report_mask = trial % 2 ? 0 : (UINT64_C(1) << REPORT_ENTRIES_STALE) |
                                  (UINT64_C(1) << REPORT_ENTRIES_TOMBSTONES);
    position = 0; preorder(nodes, 5, SIZE_MAX);
    CHECK(position == NODES);
    for (size_t i = 0; i < NODES; i++) {
      nodes[i].index = subtree_create(); nodes[i].valid = &valid[i];
      for (unsigned k = 0; k < KEYS; k++) {
        if (!(present[i][k] = draw() % 3 != 0)) continue;
        uint64_t key = key_at(k);
        marked[i][k] = draw() % 5 == 0;
        tomb[i][k] = draw() % 4 == 0;
        shy[i][k] = draw() % 3 == 0;
        index_entry_t e = {.slab_idx=k | (marked[i][k] ? SIDX_INVALID_BIT : 0) |
                                       (shy[i][k] ? SIDX_SHY_BIT : 0)};
        subtree_insert(nodes[i].index, (unsigned char *)&key, 8, &e);
        subtree_report_record(nodes[i].index, key, k, tomb[i][k]);
        valid[i] += !marked[i][k];
      }
    }
    // Independent oracle: every occurrence checks ALL its ancestors. Sibling
    // occurrences deliberately overlap, but must never invalidate each other.
    for (size_t i = 0; i < NODES; i++) for (unsigned k = 0; k < KEYS; k++) {
      if (!present[i][k]) continue;
      for (size_t p = nodes[i].parent; p != SIZE_MAX; p = nodes[p].parent)
        if (present[p][k] && !marked[p][k]) { marked[p][k] = 1; expected_new++; }
    }
    CHECK(subtree_invalidate_idle(nodes, NODES, tick, &ticks, &observed) == 0);
    CHECK(observed == expected_new && ticks > 0);
    for (size_t i = 0; i < NODES; i++) {
      uint64_t want_total = 0, want_stale = 0, want_tomb = 0, total = 0, stale = 0, tombs = 0;
      for (unsigned k = 0; k < KEYS; k++) {
        uint64_t key = key_at(k); index_entry_t e;
        CHECK(subtree_find(nodes[i].index, (unsigned char *)&key, 8, &e) == present[i][k]);
        if (!present[i][k]) continue;
        CHECK(GET_SIDX(e.slab_idx) == k && !!sidx_is_shy(e.slab_idx) == shy[i][k]);
        CHECK(!!sidx_is_invalid(e.slab_idx) == marked[i][k]);
        want_total++; want_stale += marked[i][k]; want_tomb += tomb[i][k] && !marked[i][k];
      }
      subtree_report_counts(nodes[i].index, &total, &stale, &tombs);
      CHECK(total == want_total && stale == want_stale);
      CHECK(tombs == (report_entry_classes() ? want_tomb : 0));
      CHECK(valid[i] == want_total - want_stale);
    }
    CHECK(subtree_invalidate_idle(nodes, NODES, NULL, NULL, &observed) == 0 && observed == 0);
    for (size_t i = 0; i < NODES; i++) subtree_free(nodes[i].index);
    CHECK(subtree_marked_total() == 0);
  }
  puts("PASS 512 histories: all ancestors, sibling isolation, tombstones, shy/sentinel keys, counts, reporting off, idempotence");
}

static void check_deep_path(void) {
  const size_t count = 10000;
  struct subtree_idle_node *nodes = calloc(count, sizeof(*nodes));
  size_t *valid = calloc(count, sizeof(*valid));
  uint64_t key = UINT64_MAX, observed;
  for (size_t i = 0; i < count; i++) {
    nodes[i] = (struct subtree_idle_node){subtree_create(), &valid[i], i ? i - 1 : SIZE_MAX};
    index_entry_t e = {0};
    subtree_insert(nodes[i].index, (unsigned char *)&key, 8, &e); valid[i] = 1;
  }
  CHECK(subtree_invalidate_idle(nodes, count, NULL, NULL, &observed) == 0);
  CHECK(observed == count - 1 && valid[count - 1] == 1);
  for (size_t i = 0; i < count; i++) { CHECK(valid[i] == (i == count - 1)); subtree_free(nodes[i].index); }
  free(nodes); free(valid);
  CHECK(subtree_invalidate_idle(NULL, 0, NULL, NULL, &observed) == 0 && observed == 0);
  puts("PASS deep path without recursion and empty tree");
}

static struct slab slabs[5];
static centree_node order[5];
static void put(unsigned n, uint64_t key) {
  struct slab *s = &slabs[n];
  W_LOCK(&s->tree_lock);
  size_t slot = s->nb_items;
  index_entry_t e = {.slab=s, .slab_idx=slot};
  subtree_insert(s->subtree, (unsigned char *)&key, 8, &e);
  slab_widen_range(s, key); s->nb_items++;
  if (n != 1 && n != 3) atomic_store(&s->last_item, s->nb_items);
  W_UNLOCK(&s->tree_lock);
}
static void *late_publication(void *unused) {
  (void)unused;
  usleep(50000);
  index_entry_t e; uint64_t key = 60;
  CHECK(subtree_find(slabs[3].subtree, (unsigned char *)&key, 8, &e));
  CHECK(!sidx_is_invalid(e.slab_idx)); // the sweep must still be waiting
  put(2, key);
  stale_invalidation_enqueue(&slabs[2], key);
  __atomic_store_n(&slabs[2].update_ref, 0, __ATOMIC_RELEASE);
  return NULL;
}

static void check_idle_handoff(void) {
  init_default_config(&cfg);
  cfg.kv_size = 1024; cfg.max_file_size = 16 * PAGE_SIZE;
  cfg.with_prune = 1; cfg.prune_stale_ratio = 1.0;
  cfg.wait_for_pruning_s = 2; cfg.timeseries_s = 0;
  cfg.with_reins = 1; cfg.with_rebal = 0;
  centree_init();
  const uint64_t pivots[5] = {49, 50, 51, 100, 101};
  for (unsigned i = 0; i < 5; i++) {
    struct slab *s = &slabs[i];
    s->seq = i + 1; s->fd = -1; s->item_size = 1024; s->nb_max_items = 64; s->min = UINT64_MAX;
    INIT_LOCK(&s->tree_lock, NULL);
    s->subtree = subtree_create(); subtree_set_slab(s->subtree, s);
    tree_entry_t v = {.key=pivots[i], .seq=s->seq, .slab=s};
    order[i] = centree_node_new((void *)(uintptr_t)pivots[i], &v); s->centree_node = order[i];
    if (i == 1 || i == 3) { atomic_store(&order[i]->child_flag, 1); atomic_store(&s->full, 1); atomic_store(&s->last_item, 64); }
  }
  order[3]->lu_child[0] = order[1]; order[3]->lu_child[1] = order[4];
  order[1]->lu_child[0] = order[0]; order[1]->lu_child[1] = order[2];
  centree_lu_parent_store(order[0], order[1]); centree_lu_parent_store(order[2], order[1]);
  centree_lu_parent_store(order[1], order[3]); centree_lu_parent_store(order[4], order[3]);
  CHECK(centree_build_from_inorder(tnt_centree(), order, 5) == 0);
  for (uint64_t k = 60; k < 100; k++) { put(3, k); put(1, k); if (k != 60) put(2, k); }
  for (uint64_t k = 500; k < 520; k++) put(3, k);
  for (int i = 0; i < 65536; i++) stale_invalidation_enqueue(&slabs[2], 999999);
  uint64_t dropped = rstats.stale_dropped;
  for (uint64_t k = 60; k < 100; k++) stale_invalidation_enqueue(&slabs[2], k);
  CHECK(rstats.stale_dropped - dropped == 40);
  slabs[2].update_ref = 1;
  uint64_t marked;
  CHECK(tnt_invalidate_idle(NULL, NULL, &marked) == -EBUSY && marked == 0);
  CHECK(slabs[3].nb_items == 60 && slabs[1].nb_items == 40);
  pthread_t publisher;
  CHECK(pthread_create(&publisher, NULL, late_publication, NULL) == 0);
  fsst_worker_init(); // idle must stop this producer before borrowing index pointers
  slab_workers_wait_for_pruning();
  CHECK(pthread_join(publisher, NULL) == 0);
  CHECK(fsst_worker_stopped() && stale_invalidation_pending() == 0);
  CHECK(slabs[3].nb_items == 20 && slabs[1].nb_items == 0 && slabs[2].nb_items == 40);
  CHECK(tnt_get_node_count() == 5); // target 1.0 suppresses pruning, not the sweep
  struct prune_candidate c;
  CHECK(prune_select(order[2], &c));
  CHECK(tnt_invalidate_idle(NULL, NULL, &marked) == 0 && marked == 0);
  puts("PASS idle repairs dropped hints below target after late publication and reinsertion stop; candidate becomes eligible without disk I/O");
}

int main(void) {
  check_random_history(); check_deep_path(); check_idle_handoff();
  return 0;
}
