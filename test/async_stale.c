/* Deterministic delayed-invalidation cases. No files or I/O workers required. */
#include "headers.h"
#include <stdbool.h>

int print, load, rc_thr = 1;

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

static struct slab *slab_new(struct slab *parent) {
  static uint64_t seq;
  struct slab *s = calloc(1, sizeof(*s));
  tree_entry_t value = {0};
  s->seq = ++seq;
  s->min = UINT64_MAX;
  s->item_size = 64;
  s->nb_max_items = 16;
  s->fd = -1;
  INIT_LOCK(&s->tree_lock, NULL);
  s->subtree = tnt_subtree_create();
  subtree_set_slab(s->subtree, s);
  value.slab = s;
  s->centree_node = centree_node_new((void *)(uintptr_t)s->seq, &value);
  centree_lu_parent_store(s->centree_node, parent ? parent->centree_node : NULL);
  return s;
}

static void seed(struct slab *s, uint64_t key, size_t slot) {
  index_entry_t e = {.slab = s, .slab_idx = slot};
  subtree_insert(s->subtree, (unsigned char *)&key, sizeof(key), &e);
  slab_widen_range(s, key);
  s->nb_items++;
}

static index_entry_t entry(struct slab *s, uint64_t key) {
  index_entry_t e;
  R_LOCK(&s->tree_lock);
  CHECK(subtree_find(s->subtree, (unsigned char *)&key, sizeof(key), &e));
  R_UNLOCK(&s->tree_lock);
  return e;
}

static void done(struct slab_callback *cb, void *item) { (void)cb; (void)item; }

static void publish(struct slab *s, uint64_t key, size_t slot, int tombstone) {
  unsigned char *item = calloc(1, 64);
  item_init((struct item_metadata *)item, sizeof(key), sizeof(key));
  memcpy(item + sizeof(struct item_metadata), &key, sizeof(key));
  if (tombstone) item_encode_tombstone((struct item_metadata *)item);
  struct slab_callback cb = {
      .slab = s, .slab_idx = slot, .item = (char *)item, .cb_cb = done};
  s->nb_items++;
  s->update_ref++;
  add_in_tree(&cb, item); /* same completion as all client appends */
  CHECK(s->update_ref == 0);
  /* Jobs must not retain this buffer or the stack callback. */
  memset(item, 0xa5, 64);
  free(item);
}

static void drain(void) {
  while (stale_invalidation_drain()) ;
  CHECK(stale_invalidation_pending() == 0);
}

static void test_marked_population(void) {
  uint64_t before = subtree_marked_total(), key = 999;
  subtree_t *t = subtree_create();
  index_entry_t e = {.slab_idx = 3};
  subtree_insert(t, (unsigned char *)&key, 8, &e);
  CHECK(subtree_set_invalid(t, (unsigned char *)&key, 8) == 1);
  CHECK(subtree_set_invalid(t, (unsigned char *)&key, 8) == 0);
  CHECK(subtree_marked_total() == before + 1);
  CHECK(subtree_delete(t, (unsigned char *)&key, 8) == 1);
  CHECK(subtree_marked_total() == before);
  e.slab_idx |= SIDX_INVALID_BIT;
  subtree_insert_shy(t, (unsigned char *)&key, 8, &e);
  subtree_insert(t, (unsigned char *)&key, 8, &e); /* duplicate adds nothing */
  CHECK(subtree_marked_total() == before + 1);
  subtree_free(t);
  CHECK(subtree_marked_total() == before);
  puts("PASS marked population tracks marking, replacement, duplicates and retirement");
}

static void test_publication(void) {
  struct slab *src = slab_new(NULL), *dst = slab_new(src);
  seed(src, 10, 0);
  publish(dst, 10, 3, 1);
  CHECK(!sidx_is_invalid(entry(src, 10).slab_idx));
  CHECK(!sidx_is_invalid(entry(dst, 10).slab_idx));
  CHECK(stale_invalidation_pending() == 1);
  CHECK(src->nb_items == 1 && dst->nb_items == 1);
  drain();
  CHECK(sidx_is_invalid(entry(src, 10).slab_idx));
  CHECK(!sidx_is_invalid(entry(dst, 10).slab_idx));
  CHECK(src->nb_items == 0 && dst->nb_items == 1);

  /* A duplicate completion cannot invalidate the same-leaf winner. */
  publish(dst, 10, 2, 0);
  CHECK(GET_SIDX(entry(dst, 10).slab_idx) == 3);
  publish(dst, 10, 4, 0);
  CHECK(GET_SIDX(entry(dst, 10).slab_idx) == 4);
  drain();
  CHECK(src->nb_items == 0 && dst->nb_items == 1);
  CHECK(!sidx_is_invalid(entry(dst, 10).slab_idx));

  /* No older copy, and the destination becomes internal before processing. */
  publish(dst, 20, 5, 0);
  (void)slab_new(dst);
  atomic_store(&dst->full, 1);
  drain();
  CHECK(!sidx_is_invalid(entry(dst, 20).slab_idx));
  puts("PASS destination-first publication, tombstones, same-leaf races, split");
}

static void test_obsolete_anchors(void) {
  struct slab *src = slab_new(NULL), *dst = slab_new(src);
  seed(src, 30, 0);
  publish(dst, 30, 0, 0);
  /* Model the exact migration swap: the node survives, its slab does not.
   * The parent's copy is now authoritative and must never be invalidated. */
  struct slab *fresh = slab_new(NULL);
  fresh->centree_node = dst->centree_node;
  ((centree_node)dst->centree_node)->value.slab = fresh;
  atomic_store(&dst->superseded, 1);
  slab_retire(dst);
  drain();
  CHECK(!sidx_is_invalid(entry(src, 30).slab_idx));

  dst = slab_new(src);
  publish(dst, 30, 0, 0);
  atomic_store(&((centree_node)dst->centree_node)->removed, 1);
  slab_retire(dst); /* frees the local index before the job is processed */
  drain();
  CHECK(!sidx_is_invalid(entry(src, 30).slab_idx));

  dst = slab_new(src);
  publish(dst, 30, 0, 0);
  uint64_t key = 30;
  subtree_set_invalid(dst->subtree, (unsigned char *)&key, sizeof(key));
  drain();
  CHECK(!sidx_is_invalid(entry(src, 30).slab_idx));
  puts("PASS migrated, retired and already-invalid anchors are skipped");
}

static void test_late_publication(void) {
  struct slab *src = slab_new(NULL), *dst = slab_new(src);
  seed(src, 70, 0);
  publish(dst, 70, 0, 0);
  drain();
  CHECK(src->nb_items == 0);
  /* A reserved write may publish into an already-internal slab after a
   * descendant's job marked its existing entry. The new unmarked entry must
   * be counted, even though its key was already indexed there. */
  publish(src, 70, 1, 0);
  CHECK(!sidx_is_invalid(entry(src, 70).slab_idx));
  CHECK(src->nb_items == 1);
  stale_invalidation_enqueue(dst, 70);
  drain();
  CHECK(src->nb_items == 0); /* must not underflow */
  puts("PASS late publication over an invalidated entry keeps counts conservative");
}

static void test_queue(void) {
  struct slab *src = slab_new(NULL), *dst = slab_new(src);
  seed(src, 40, 0);
  publish(dst, 40, 0, 0);
  /* A waiting hint must not pin update_ref and block this same consumer. */
  CHECK(slab_freeze(dst, dst->nb_max_items) >= 0);
  uint64_t before = rstats.stale_dropped;
  for (size_t i = 0; i < 70000; i++)
    stale_invalidation_enqueue(dst, 40);
  CHECK(rstats.stale_dropped > before);
  CHECK(stale_invalidation_pending() <= 65536);
  CHECK(dst->update_ref == 0 && src->update_ref == 0);
  CHECK(stale_invalidation_drain() == 128);
  CHECK(stale_invalidation_pending() > 0);
  drain();
  CHECK(src->nb_items == 0); /* repeated jobs decrement exactly once */
  CHECK(!sidx_is_invalid(entry(dst, 40).slab_idx));
  puts("PASS bounded queue, overflow, batch limit, no pinned write references");
}

static void *producer(void *arg) {
  for (size_t i = 0; i < 2000; i++)
    stale_invalidation_enqueue(arg, 50);
  return NULL;
}

static void test_producers(void) {
  struct slab *src = slab_new(NULL), *dst = slab_new(src);
  pthread_t threads[4];
  seed(src, 50, 0);
  seed(dst, 50, 0);
  uint64_t queued = rstats.stale_queued, dropped = rstats.stale_dropped;
  for (int i = 0; i < 4; i++) CHECK(pthread_create(&threads[i], NULL, producer, dst) == 0);
  for (int i = 0; i < 4; i++) pthread_join(threads[i], NULL);
  CHECK(rstats.stale_queued - queued + rstats.stale_dropped - dropped == 8000);
  drain();
  CHECK(src->nb_items == 0 && dst->nb_items == 1);
  puts("PASS concurrent producers, dropped-job accounting, idempotent marking");
}

static void test_worker(void) {
  struct slab *src = slab_new(NULL), *dst = slab_new(src);
  seed(src, 60, 0);
  /* The utilization gate is closed, and no structural feature is enabled. */
  cfg.util_gate = 1;
  cfg.maintenance_period_ms = 60000;
  CHECK(restructuring_worker_init() == 0);
  usleep(20000);
  uint64_t processed = rstats.stale_processed;
  for (int i = 0; i < 1000; i++) {
    stale_invalidation_enqueue(dst, 60); /* unpublished destination: discard */
    if (stale_invalidation_pending()) break;
  }
  for (int i = 0; i < 1000 && (stale_invalidation_pending() ||
       __atomic_load_n(&rstats.stale_processed, __ATOMIC_RELAXED) == processed); i++)
    usleep(1000);
  CHECK(stale_invalidation_pending() == 0);
  CHECK(__atomic_load_n(&rstats.stale_processed, __ATOMIC_RELAXED) > processed);
  CHECK(!sidx_is_invalid(entry(src, 60).slab_idx));
  publish(dst, 60, 0, 0);
  for (int i = 0; i < 1000 && !sidx_is_invalid(entry(src, 60).slab_idx); i++)
    usleep(1000);
  CHECK(sidx_is_invalid(entry(src, 60).slab_idx));
  puts("PASS queue wakes idle worker independently of period and utilization gate");
}

struct request {
  struct slab_callback cb;
  unsigned char item[64];
  _Atomic int done;
  uint64_t value;
};

static void request_done(struct slab_callback *cb, void *item) {
  struct request *r = (struct request *)cb;
  CHECK(item != NULL);
  memcpy(&r->value, (char *)item + sizeof(struct item_metadata) + 8, 8);
  atomic_store_explicit(&r->done, 1, memory_order_release);
}

static uint64_t request(uint64_t key, uint64_t value, int read) {
  struct request r = {0};
  item_init((struct item_metadata *)r.item, 8, 8);
  memcpy(r.item + sizeof(struct item_metadata), &key, 8);
  memcpy(r.item + sizeof(struct item_metadata) + 8, &value, 8);
  r.cb.item = (char *)r.item;
  r.cb.cb = request_done;
  r.cb.fsst_idx = UINT64_MAX;
  if (read) kv_read_async(&r.cb); else kv_upsert_async(&r.cb);
  for (int i = 0; i < 5000 && !atomic_load_explicit(&r.done, memory_order_acquire); i++)
    usleep(1000);
  CHECK(atomic_load_explicit(&r.done, memory_order_acquire));
  return r.value;
}

static void wait_pending(void) {
  for (int i = 0; i < 5000 && stale_invalidation_pending(); i++) usleep(1000);
  CHECK(stale_invalidation_pending() == 0);
}

static void test_pipeline(char *directory) {
  cfg.directory = directory;
  cfg.kv_size = 64;
  cfg.max_file_size = PAGE_SIZE;
  cfg.page_cache_size = PAGE_SIZE * 8192;
  cfg.maintenance_period_ms = 60000;
  cfg.util_gate = 1;
  slab_workers_init(1, 2, 2); /* no explicit structural-worker initialization */
  for (uint64_t k = 0; k < 128; k++) request(k, k, 0);
  wait_pending();

  unsigned char query[64] = {0};
  item_init((struct item_metadata *)query, 8, 8); /* key zero */
  struct slab_callback cb = {.item = (char *)query};
  index_entry_t *e = tnt_index_lookup(&cb, query);
  CHECK(e != NULL);
  struct slab *old = e->slab;
  tnt_index_lookup_unref(e);
  CHECK(atomic_load(&old->full));
  CHECK(!sidx_is_invalid(entry(old, 0).slab_idx));

  tnt_maintenance_lock(); /* queued invalidation cannot proceed */
  CHECK(request(0, 999, 0) == 999);
  CHECK(request(0, 0, 1) == 999);
  CHECK(!sidx_is_invalid(entry(old, 0).slab_idx));
  CHECK(stale_invalidation_pending() > 0);
  tnt_maintenance_unlock();
  wait_pending();
  CHECK(sidx_is_invalid(entry(old, 0).slab_idx));
  CHECK(request(0, 0, 1) == 999);
  puts("PASS real moving write/read complete while maintenance is blocked");
}

int main(int argc, char **argv) {
  init_default_config(&cfg);
  cfg.with_rebal = cfg.with_prune = 0;
  if (argc == 3 && !strcmp(argv[1], "pipeline")) {
    test_pipeline(argv[2]);
    return 0;
  }
  centree_init();
  test_marked_population();
  test_publication();
  test_obsolete_anchors();
  test_late_publication();
  test_queue();
  test_producers();
  test_worker();
  puts("All async stale tests passed");
  return 0;
}
