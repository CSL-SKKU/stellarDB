/*
 * Targeted DELETE edge cases. Each sub-command runs as its own process so a
 * die()/assert() in one case does not hide the others.
 *
 *   count    - what get_database_size() reports once every key is deleted.
 *              Reported for information only: the value has no consistent
 *              meaning, so nothing here asserts on it. The one invariant
 *              checked is that no deleted key is readable.
 *   ondisk   - scan the slab files and validate the persisted tombstone layout
 *   reins    - force background reinsertion over slabs holding tombstones
 *   readd    - kv_add_async() on a key that was deleted
 *   reuse    - reuse one item buffer for DELETE and then for UPSERT
 *   inplace  - deletes that reuse the existing slot (no split in between)
 *   narrow   - fill a leaf that routes only a couple of distinct keys
 */
#include "headers.h"

#include <stdbool.h>
#include <stdarg.h>

int print = 0;
int load = 0;
int rc_thr = 1;

#define TEST_KV_SIZE 128
#define TEST_MAX_FILE_SIZE (32LU * PAGE_SIZE)

static _Atomic uint64_t failures;

static void fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("FAIL ");
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
  atomic_fetch_add(&failures, 1);
}

struct req {
  struct slab_callback cb; /* first */
  uint64_t key;
  uint64_t expect_value;
  int expect_found;
  int saw_item;
  uint64_t saw_key;
  uint64_t saw_value;
  _Atomic int done;
};

static unsigned char *make_item(uint64_t key, uint64_t value) {
  unsigned char *item = calloc(1, TEST_KV_SIZE);
  struct item_metadata *meta = (struct item_metadata *)item;
  item_init(meta, sizeof(uint64_t),
            TEST_KV_SIZE - sizeof(*meta) - sizeof(uint64_t));
  *(uint64_t *)(item + sizeof(*meta)) = key;
  *(uint64_t *)(item + sizeof(*meta) + sizeof(uint64_t)) = value;
  return item;
}

static void read_done(struct slab_callback *cb, void *item) {
  struct req *r = (struct req *)cb;
  struct item_metadata *meta = item;
  if (item) {
    unsigned char *rec = item;
    r->saw_item = 1;
    r->saw_key = *(uint64_t *)(rec + sizeof(*meta));
    r->saw_value = *(uint64_t *)(rec + sizeof(*meta) + sizeof(uint64_t));
  }
  atomic_store(&r->done, 1);
}

static void write_done(struct slab_callback *cb, void *item) {
  (void)item;
  atomic_store(&((struct req *)cb)->done, 1);
}

static void init_req(struct req *r, unsigned char *item, uint64_t key,
                     slab_cb_t *done) {
  memset(r, 0, sizeof(*r));
  r->cb.cb = done;
  r->cb.item = (char *)item;
  r->cb.fsst_idx = -1;
  r->key = key;
  atomic_store(&r->done, 0);
}

static void wait_one(struct req *r) {
  while (!atomic_load(&r->done)) NOP10();
}

enum { OP_ADD, OP_UPSERT, OP_DELETE };

static void do_write_item(unsigned char *item, uint64_t key, int op) {
  struct req r;
  init_req(&r, item, key, write_done);
  if (op == OP_ADD) kv_add_async(&r.cb);
  else if (op == OP_UPSERT) kv_upsert_async(&r.cb);
  else kv_remove_async(&r.cb);
  wait_one(&r);
}

static void do_write(uint64_t key, uint64_t value, int op) {
  unsigned char *item = make_item(key, value);
  do_write_item(item, key, op);
  free(item);
}

/* returns 1 when the key is visible; *value gets the value read */
static int do_read(uint64_t key, uint64_t *value) {
  struct req r;
  unsigned char *item = make_item(key, 0);
  init_req(&r, item, key, read_done);
  kv_read_async(&r.cb);
  wait_one(&r);
  free(item);
  if (r.saw_item && value) *value = r.saw_value;
  if (r.saw_item && r.saw_key != key)
    fail("read of key %lu returned key %lu", key, r.saw_key);
  return r.saw_item;
}

static uint64_t max_file_size = TEST_MAX_FILE_SIZE;

static void boot(int reins) {
  init_default_config(&cfg);
  cfg.kv_size = TEST_KV_SIZE;
  cfg.max_file_size = max_file_size;
  cfg.page_cache_size = PAGE_SIZE * 8192;
  cfg.with_reins = reins;
  cfg.with_rebal = 0;
  cfg.epoch = 1000;
  slab_workers_init(1, 4, 2);
  if (reins) fsst_worker_init();
}

/* ------------------------------------------------------------------ cases */

static void case_count(uint64_t n) {
  for (uint64_t k = 0; k < n; k++) do_write(k, k + 1, OP_ADD);
  printf("  after %lu inserts   get_database_size() = %lu\n", n,
         get_database_size());
  for (uint64_t k = 0; k < n; k++) do_write(k, 0, OP_DELETE);
  uint64_t after = get_database_size();
  printf("  after %lu deletes   get_database_size() = %lu\n", n, after);

  uint64_t visible = 0;
  for (uint64_t k = 0; k < n; k++) visible += do_read(k, NULL);
  printf("  readable keys       = %lu\n", visible);
  if (visible != 0) fail("%lu keys still readable after deleting all", visible);
  /*
   * `after` stays at 2n rather than dropping to 0. Do not assert anything
   * about it: nb_totals counts published records at runtime but is rebuilt as
   * distinct keys by recovery, so it does not measure one fixed quantity and
   * in particular is not the number of readable keys.
   */

  /* deleting keys that never existed */
  for (uint64_t k = n; k < n + 100; k++) do_write(k, 0, OP_DELETE);
  printf("  after 100 deletes of absent keys  get_database_size() = %lu\n",
         get_database_size());
}

/* Walk the slab files and check the persisted tombstone representation. */
static void case_ondisk(uint64_t n) {
  for (uint64_t k = 0; k < n; k++) do_write(k, k + 1, OP_ADD);
  for (uint64_t k = 0; k < n; k += 2) do_write(k, 0, OP_DELETE);

  size_t tombs = 0, normals = 0, bad_size = 0, bad_key = 0, residue = 0;
  size_t nb_slabs = 0;
  char *buf = aligned_alloc(PAGE_SIZE, max_file_size);

  for (int seq = 0;; seq++) {
    tree_entry_t *e = tnt_traverse_use_seq(seq);
    if (!e) break;
    struct slab *s = e->slab;
    nb_slabs++;
    ssize_t r = pread(s->fd, buf, slab_data_size(s), 0); /* slots only */
    if (r <= 0) continue;
    size_t items_per_page = PAGE_SIZE / s->item_size;
    for (size_t p = 0; p * PAGE_SIZE < (size_t)r; p++) {
      for (size_t i = 0; i < items_per_page; i++) {
        unsigned char *rec =
            (unsigned char *)buf + p * PAGE_SIZE + i * s->item_size;
        struct item_metadata *meta = (struct item_metadata *)rec;
        if (item_is_empty(meta)) continue;
        if (item_is_legacy(meta)) {
          fail("legacy metadata persisted in slab %lu", s->seq);
          continue;
        }
        uint64_t key = *(uint64_t *)(rec + sizeof(*meta));
        if (item_is_tombstone(meta)) {
          tombs++;
          if (item_key_size(meta) != sizeof(uint64_t)) bad_key++;
          if (meta->value_size != 0) bad_size++;
          if (item_stored_size(meta) != sizeof(*meta) + sizeof(uint64_t))
            bad_size++;
          /* AGENTS.md: a tombstone record is metadata + key, no value */
          uint64_t stale = *(uint64_t *)(rec + sizeof(*meta) + sizeof(uint64_t));
          if (stale == key + 1) residue++;
          (void)key;
        } else {
          normals++;
        }
      }
    }
  }
  printf("  slabs=%lu  tombstones=%lu  live records=%lu\n", nb_slabs, tombs,
         normals);
  if (tombs < n / 2)
    fail("expected >= %lu persisted tombstones, found %lu", n / 2, tombs);
  if (bad_key) fail("%lu tombstones lost their key size", bad_key);
  if (bad_size) fail("%lu tombstones have a non-zero value size", bad_size);
  printf("  tombstone slots still holding the old value bytes: %lu\n", residue);
  free(buf);
}

/* Force the background reinsertion worker over every slab (step 7). */
static void case_reins(uint64_t n) {
  for (uint64_t k = 0; k < n; k++) do_write(k, k + 1, OP_ADD);
  for (uint64_t k = 0; k < n; k += 2) do_write(k, 0, OP_DELETE);

  size_t queued = 0;
  for (int seq = 0;; seq++) {
    tree_entry_t *e = tnt_traverse_use_seq(seq);
    if (!e) break;
    struct slab *s = e->slab;
    if (!s->hot_bits) continue;
    size_t nb_pages = (s->size_on_disk + PAGE_SIZE - 1) / PAGE_SIZE;
    for (size_t p = 0; p < nb_pages; p++) mark_page_hot(s, p);
    if (!atomic_load(&s->queued)) {
      atomic_store(&s->queued, 1);
      bgq_enqueue(GC, s);
      queued++;
    }
  }
  printf("  queued %lu slabs for reinsertion\n", queued);

  for (int i = 0; i < 60 && !bgq_is_empty(GC); i++) sleep(1);
  sleep(3); /* let the in-flight copy-forwards drain */
  printf("  reinsertion queue drained (empty=%d)\n", bgq_is_empty(GC));

  uint64_t wrong = 0, resurrected = 0, lost = 0;
  for (uint64_t k = 0; k < n; k++) {
    uint64_t v = 0;
    int found = do_read(k, &v);
    if (k % 2 == 0) {
      if (found) { resurrected++; if (resurrected <= 5) printf("  resurrected key %lu = %lu\n", k, v); }
    } else {
      if (!found) { lost++; if (lost <= 5) printf("  lost key %lu\n", k); }
      else if (v != k + 1) { wrong++; if (wrong <= 5) printf("  key %lu has value %lu, expected %lu\n", k, v, k + 1); }
    }
  }
  if (resurrected) fail("%lu deleted keys came back after reinsertion", resurrected);
  if (lost) fail("%lu live keys disappeared after reinsertion", lost);
  if (wrong) fail("%lu live keys have a corrupted value after reinsertion", wrong);
}

static void case_readd(uint64_t n) {
  (void)n;
  do_write(1, 111, OP_ADD);
  do_write(1, 0, OP_DELETE);
  if (do_read(1, NULL)) fail("key 1 still visible after DELETE");
  printf("  about to kv_add_async() a deleted key...\n");
  fflush(stdout);
  do_write(1, 222, OP_ADD);
  uint64_t v = 0;
  if (!do_read(1, &v))
    fail("key 1 not visible after re-ADD");
  else if (v != 222)
    fail("key 1 has value %lu after re-ADD, expected 222", v);
  else
    printf("  re-ADD of a deleted key worked\n");
}

/* A client that keeps one item buffer around and reuses it. */
static void case_reuse(uint64_t n) {
  (void)n;
  unsigned char *item = make_item(5, 555);
  do_write_item(item, 5, OP_ADD);
  if (!do_read(5, NULL)) fail("key 5 not visible after insert");

  printf("  DELETE with the buffer, then UPSERT with the same buffer...\n");
  fflush(stdout);
  do_write_item(item, 5, OP_DELETE); /* engine rewrites the metadata in place */
  if (do_read(5, NULL)) fail("key 5 visible after DELETE");

  /* The caller believes it still holds a normal {5,555} record. */
  struct item_metadata *meta = (struct item_metadata *)item;
  printf("  caller's buffer after DELETE: tombstone=%d key_size=%lu "
         "value_size=%lu\n",
         item_is_tombstone(meta) ? 1 : 0, item_key_size(meta),
         meta->value_size);
  do_write_item(item, 5, OP_UPSERT);
  uint64_t v = 0;
  if (!do_read(5, &v))
    fail("key 5 is still invisible after re-UPSERT with the reused buffer");
  else if (v != 555)
    fail("key 5 has value %lu after re-UPSERT, expected 555", v);
  else
    printf("  reused buffer round-tripped fine\n");

  printf("  now DELETE twice with the same buffer...\n");
  fflush(stdout);
  do_write_item(item, 5, OP_DELETE);
  do_write_item(item, 5, OP_DELETE);
  printf("  double DELETE with a reused buffer survived\n");
  free(item);
}

/*
 * With a slab big enough that no split happens, a DELETE of a key that lives
 * in the current (non-full) leaf takes the in-place branch of
 * centree_lookup_and_reserve(): the tombstone overwrites the record's slot.
 */
static void case_inplace(uint64_t n) {
  for (uint64_t k = 0; k < n; k++) do_write(k, k + 1, OP_ADD);
  for (uint64_t k = 0; k < n; k++) do_write(k, 0, OP_DELETE);

  uint64_t visible = 0;
  for (uint64_t k = 0; k < n; k++) visible += do_read(k, NULL);
  if (visible) fail("%lu keys readable after in-place delete", visible);

  size_t tombs = 0, normals = 0, slabs = 0, residue = 0;
  char *buf = aligned_alloc(PAGE_SIZE, max_file_size);
  for (int seq = 0;; seq++) {
    tree_entry_t *e = tnt_traverse_use_seq(seq);
    if (!e) break;
    struct slab *s = e->slab;
    slabs++;
    ssize_t r = pread(s->fd, buf, slab_data_size(s), 0); /* slots only */
    if (r <= 0) continue;
    size_t ipp = PAGE_SIZE / s->item_size;
    for (size_t p = 0; p * PAGE_SIZE < (size_t)r; p++)
      for (size_t i = 0; i < ipp; i++) {
        unsigned char *rec =
            (unsigned char *)buf + p * PAGE_SIZE + i * s->item_size;
        struct item_metadata *meta = (struct item_metadata *)rec;
        if (item_is_empty(meta)) continue;
        uint64_t key = *(uint64_t *)(rec + sizeof(*meta));
        if (item_is_tombstone(meta)) {
          tombs++;
          if (*(uint64_t *)(rec + sizeof(*meta) + sizeof(uint64_t)) == key + 1)
            residue++;
        } else {
          normals++;
        }
      }
  }
  free(buf);
  printf("  slabs=%lu tombstones=%lu live=%lu\n", slabs, tombs, normals);
  printf("  tombstones that reused an existing slot (old value still on "
         "disk): %lu\n", residue);
  if (tombs != n)
    fail("expected %lu tombstones on disk, found %lu", n, tombs);
  if (normals != 0)
    fail("%lu original records survived; deletes did not reuse their slot",
         normals);
}

/*
 * Probe the split pivot when a leaf routes a very narrow key range.
 *
 * Several writes for one key can be in flight at once: the in-place branch of
 * centree_lookup_and_reserve() only fires once a previous version has been
 * published, so every racing writer reserves a slot of its own. A leaf routing
 * only keys {0,1} can therefore fill while having observed exactly that range,
 * and
 *     new_key = min + (max - min) / 2
 * is then 0, whose left child would be keyed 0 - 1 == UINT64_MAX. This is
 * reachable whether or not min/max are published at reservation time, so it
 * says nothing about that change on its own -- it asks what the split does
 * when the range is genuinely narrow rather than genuinely unpublished.
 */
#define NARROW_KEYSPACE 4
#define NARROW_THREADS 8

struct narrow_arg {
  int id;
  uint64_t ops;
};

static struct req *narrow_reqs;
static unsigned char **narrow_items;

static void *narrow_worker(void *p) {
  struct narrow_arg *a = p;
  pin_me_on(get_nb_workers() + get_nb_distributors() + a->id);
  for (uint64_t i = 0; i < a->ops; i++) {
    uint64_t slot = (uint64_t)a->id * a->ops + i;
    uint64_t k = slot % NARROW_KEYSPACE;
    narrow_items[slot] = make_item(k, slot);
    init_req(&narrow_reqs[slot], narrow_items[slot], k, write_done);
    kv_upsert_async(&narrow_reqs[slot].cb);
  }
  return NULL;
}

static void case_narrow(uint64_t n) {
  uint64_t per = n / NARROW_THREADS;
  pthread_t t[NARROW_THREADS];
  struct narrow_arg a[NARROW_THREADS];

  n = per * NARROW_THREADS;
  narrow_reqs = calloc(n, sizeof(*narrow_reqs));
  narrow_items = calloc(n, sizeof(*narrow_items));
  if (!narrow_reqs || !narrow_items) {
    fail("out of memory");
    return;
  }

  printf("  firing %lu concurrent upserts over keys 0..%d from %d threads\n",
         n, NARROW_KEYSPACE - 1, NARROW_THREADS);
  fflush(stdout);
  for (int i = 0; i < NARROW_THREADS; i++) {
    a[i].id = i;
    a[i].ops = per;
    pthread_create(&t[i], NULL, narrow_worker, &a[i]);
  }
  for (int i = 0; i < NARROW_THREADS; i++) pthread_join(t[i], NULL);
  for (uint64_t i = 0; i < n; i++) {
    while (!atomic_load(&narrow_reqs[i].done)) NOP10();
    free(narrow_items[i]);
  }
  printf("  burst completed\n");

  size_t slabs = 0, narrow_slabs = 0;
  for (int seq = 0;; seq++) {
    tree_entry_t *e = tnt_traverse_use_seq(seq);
    if (!e) break;
    struct slab *s = e->slab;
    slabs++;
    if (s->min != (uint64_t)-1 && s->max - s->min <= 1) narrow_slabs++;
    if (slabs <= 20)
      printf("    slab seq=%lu key=%lu min=%lu max=%lu full=%d last_item=%lu\n",
             s->seq, s->key, s->min, s->max, atomic_load(&s->full),
             (size_t)atomic_load(&s->last_item));
  }
  printf("  %lu slabs, %lu of them routing a range of <= 2 keys\n", slabs,
         narrow_slabs);

  /* Values race, so only readability is checkable. */
  for (uint64_t k = 0; k < NARROW_KEYSPACE; k++)
    if (!do_read(k, NULL))
      fail("key %lu is unreadable after the narrow-range burst", k);
}

int main(int argc, char **argv) {
  const char *what = argc > 1 ? argv[1] : "count";
  uint64_t n = argc > 2 ? strtoull(argv[2], NULL, 0) : 3000;
  if (getenv("E2E_MAX_FILE_SIZE"))
    max_file_size = strtoull(getenv("E2E_MAX_FILE_SIZE"), NULL, 0);

  printf("== delete edge case: %s (n=%lu) ==\n", what, n);
  boot(!strcmp(what, "reins"));

  if (!strcmp(what, "count")) case_count(n);
  else if (!strcmp(what, "ondisk")) case_ondisk(n);
  else if (!strcmp(what, "reins")) case_reins(n);
  else if (!strcmp(what, "readd")) case_readd(n);
  else if (!strcmp(what, "reuse")) case_reuse(n);
  else if (!strcmp(what, "inplace")) case_inplace(n);
  else if (!strcmp(what, "narrow")) case_narrow(n);
  else { printf("unknown case %s\n", what); return 2; }

  uint64_t f = atomic_load(&failures);
  printf(f ? "\n== %lu FAILURES ==\n" : "\n== ok ==\n", f);
  return f ? EXIT_FAILURE : EXIT_SUCCESS;
}
