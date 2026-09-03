/*
 * Regression test for the split decision in add_item_async_cb1().
 *
 * A moving UPSERT that reserves the final slot of a leaf must split it. The
 * split test used to be
 *
 *     slab_idx + 1 == nb_max_items && slab_idx != fsst_idx
 *
 * meant to exclude in-place writes, but fsst_idx is the *source* slot in a
 * different slab. When the source slot and the destination's final slot had
 * the same index the writer declined to split, leaving a full, childless leaf
 * on which every later writer to its range parks on child_flag forever. Fixed
 * by also requiring fsst_slab == s (AGENTS.md, Known bugs 23).
 *
 * Layout: kv 128 B, one data page per slab -> M = 32 slots.
 *   1. keys 1..M fill the root; key M lands in root slot M-1 and splits it.
 *   2. M-1 new keys > pivot fill slots 0..M-2 of the right child.
 *   3. UPSERT key M: reserves right slot M-1, old copy is root slot M-1.
 *      (CONTROL=1 upserts key M-1 instead, source slot M-2: a setup check.)
 *   4. UPSERT another key in the same range: must complete.
 *
 * Exit 0 when the leaf split and the late writer completed, 1 otherwise.
 */
#include "headers.h"

#include <stdarg.h>
#include <stdbool.h>

int print = 0;
int load = 0;
int rc_thr = 1;

#define KV 128
#define FILE_SZ (1LU * PAGE_SIZE)

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
  unsigned char *item = calloc(1, KV);
  struct item_metadata *meta = (struct item_metadata *)item;

  item_init(meta, sizeof(uint64_t), KV - sizeof(*meta) - sizeof(uint64_t));
  *(uint64_t *)(item + sizeof(*meta)) = key;
  *(uint64_t *)(item + sizeof(*meta) + sizeof(uint64_t)) = value;
  return item;
}

static struct req *upsert_async(uint64_t key, uint64_t value) {
  struct req *r = calloc(1, sizeof(*r));

  r->item = make_item(key, value);
  r->cb.cb = write_done;
  r->cb.item = (char *)r->item;
  r->cb.fsst_slab = NULL;
  r->cb.fsst_idx = -1;
  kv_upsert_async(&r->cb);
  return r;
}

static bool wait_done(struct req *r, int ms) {
  for (int i = 0; i < ms && !atomic_load(&r->done); i++) usleep(1000);
  return atomic_load(&r->done);
}

static void upsert(uint64_t key, uint64_t value) {
  struct req *r = upsert_async(key, value);

  if (!wait_done(r, 5000)) {
    printf("FAIL: upsert of key %lu did not complete in 5 s\n", key);
    _exit(2);
  }
  free(r->item);
  free(r);
}

static centree_node routing_root(void) {
  background_queue q;

  init_queue(&q);
  if (tnt_get_nodes_at_level(0, &q) != 1) return NULL;
  return dequeue_centnode(&q);
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

static void show(const char *what, centree_node n) {
  struct slab *s = n->value.slab;

  printf("%-28s slab %lu: last_item=%zu/%zu full=%d child_flag=%d "
         "routing children=%s/%s\n",
         what, s->seq, (size_t)atomic_load(&s->last_item), s->nb_max_items,
         (int)atomic_load(&s->full), (int)atomic_load(&n->child_flag),
         tnt_routing_left(n) ? "yes" : "NULL",
         tnt_routing_right(n) ? "yes" : "NULL");
}

int main(void) {
  centree_node root, leaf;
  struct slab *s;
  size_t M;
  struct req *late;
  int bug = 0;

  setvbuf(stdout, NULL, _IONBF, 0);
  init_default_config(&cfg);
  cfg.kv_size = KV;
  cfg.max_file_size = FILE_SZ;
  cfg.page_cache_size = PAGE_SIZE * 1024;
  cfg.with_reins = 0;
  cfg.with_rebal = 0;
  cfg.with_prune = 0;
  cfg.nb_items_in_db = 1000;
  slab_workers_init(1, 1, 1);

  root = routing_root();
  M = root->value.slab->nb_max_items;
  printf("slots per slab M=%zu\n", M);

  /* 1. keys 1..M: key k -> root slot k-1; key M takes the last slot, splits. */
  for (uint64_t k = 1; k <= M; k++) upsert(k, k);
  show("root after fill", root);
  if (atomic_load(&root->child_flag) != 1) {
    printf("FAIL: root did not split\n");
    return 3;
  }

  /* 2. M-1 fresh keys above the pivot go to the right child, slots 0..M-2. */
  leaf = leaf_for_key(M + 1);
  for (uint64_t k = M + 1; k <= 2 * M - 1; k++) upsert(k, k);
  show("right leaf, M-1 slots used", leaf);
  if (leaf_for_key(M) != leaf ||
      atomic_load(&leaf->value.slab->last_item) != M - 1) {
    printf("FAIL: setup did not produce the expected leaf state\n");
    return 3;
  }

  /* 3. Moving UPSERT of key M: destination slot M-1, source root slot M-1. */
  /* CONTROL=1: source is root slot M-2 instead, so the indices differ. */
  upsert(getenv("CONTROL") ? M - 1 : M, 0xbeef);
  s = leaf->value.slab;
  show("right leaf after key M", leaf);
  if (atomic_load(&s->full) && atomic_load(&leaf->child_flag) == 0 &&
      tnt_routing_left(leaf) == NULL && tnt_routing_right(leaf) == NULL) {
    printf("BUG: final slot reserved, slab full, but no split happened\n");
    bug = 1;
  }

  /* 4. Any further write to this range: routes to the leaf and must finish. */
  late = upsert_async(2 * M, 1);
  if (wait_done(late, 3000)) {
    printf("late writer completed (leaf split after all)\n");
  } else {
    printf("BUG: writer to key %zu parked for 3 s on the childless full leaf "
           "(distributor stuck)\n", 2 * M);
    bug = 1;
  }

  printf(bug ? "== FAIL: split skipped ==\n" : "== ok ==\n");
  _exit(bug);
}
