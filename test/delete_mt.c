/*
 * Concurrent DELETE / UPSERT / READ test.
 *
 * Every key is owned by exactly one client thread, and a thread waits for a
 * request to complete before issuing the next one on that key. Per-key
 * ordering is therefore well defined even though the engine gives no
 * request-level ordering across distributors, so a simple in-memory oracle can
 * check every read.
 *
 * Meanwhile all threads hammer the same center tree, so splits, moving
 * updates, tombstone publication and (optionally) background reinsertion all
 * run concurrently.
 *
 * Usage: ./test/test_delete_mt [nb_keys] [nb_threads] [ops_per_thread]
 *        E2E_REINS=1 enables background reinsertion.
 */
#include "headers.h"

#include <stdbool.h>
#include <stdarg.h>

int print = 0;
int load = 0;
int rc_thr = 1;

#define TEST_KV_SIZE 128
#define TEST_MAX_FILE_SIZE (32LU * PAGE_SIZE)

static uint64_t nb_keys = 20000;
static int nb_threads = 8;
static uint64_t ops_per_thread = 20000;

static int nb_io_workers = 8;
static int nb_distributors = 4;

static int delete_pct = 40; /* E2E_DELETE_PCT=0 turns the test into pure upserts */

static _Atomic uint64_t failures;
#define FAILN 32
static char fail_msg[FAILN][256];
static _Atomic int fail_msgs;

static void record_failure(const char *fmt, ...) {
  int slot = atomic_fetch_add(&fail_msgs, 1);
  atomic_fetch_add(&failures, 1);
  if (slot < FAILN) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fail_msg[slot], sizeof(fail_msg[slot]), fmt, ap);
    va_end(ap);
  }
}

/* oracle */
static unsigned char *exists;   /* 1 if the key must be readable */
static uint64_t *values;

struct req {
  struct slab_callback cb; /* must stay first */
  uint64_t key;
  uint64_t expect_value;
  int expect_found;
  const char *phase;
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

  if (!item) {
    if (r->expect_found)
      record_failure("[%s] key %lu: READ miss, expected value %lu", r->phase,
                     r->key, r->expect_value);
  } else {
    unsigned char *rec = item;
    uint64_t got_key = *(uint64_t *)(rec + sizeof(*meta));
    uint64_t got_val = *(uint64_t *)(rec + sizeof(*meta) + sizeof(uint64_t));
    if (!r->expect_found)
      record_failure("[%s] key %lu: resurrected (key %lu val %lu), expected "
                     "a tombstone miss",
                     r->phase, r->key, got_key, got_val);
    else if (got_key != r->key)
      record_failure("[%s] key %lu: READ returned wrong key %lu", r->phase,
                     r->key, got_key);
    else if (got_val != r->expect_value)
      record_failure("[%s] key %lu: READ value %lu, expected %lu", r->phase,
                     r->key, got_val, r->expect_value);
    if (item_is_tombstone(meta))
      record_failure("[%s] key %lu: tombstone leaked to user callback",
                     r->phase, r->key);
  }
  atomic_store(&r->done, 1);
}

static void write_done(struct slab_callback *cb, void *item) {
  (void)item;
  atomic_store(&((struct req *)cb)->done, 1);
}

static void init_req(struct req *r, unsigned char *item, uint64_t key,
                     slab_cb_t *done, const char *phase) {
  memset(&r->cb, 0, sizeof(r->cb));
  r->cb.cb = done;
  r->cb.item = (char *)item;
  r->cb.fsst_idx = -1;
  r->key = key;
  r->phase = phase;
  atomic_store(&r->done, 0);
}

static void wait_one(struct req *r) {
  while (!atomic_load(&r->done)) NOP10();
}

/* Blocking single-key helpers. */
static void do_write(uint64_t key, uint64_t value, int op, const char *phase) {
  struct req r;
  unsigned char *item = make_item(key, value);
  init_req(&r, item, key, write_done, phase);
  if (op == 0)
    kv_add_async(&r.cb);
  else if (op == 1)
    kv_upsert_async(&r.cb);
  else
    kv_remove_async(&r.cb);
  wait_one(&r);
  free(item);
}

static void do_read(uint64_t key, int expect_found, uint64_t expect_value,
                    const char *phase) {
  struct req r;
  unsigned char *item = make_item(key, 0);
  init_req(&r, item, key, read_done, phase);
  r.expect_found = expect_found;
  r.expect_value = expect_value;
  kv_read_async(&r.cb);
  wait_one(&r);
  free(item);
}

struct targ {
  int id;
  unsigned int seed;
};

/*
 * Optional: keep shoving every slab into the reinsertion queue so that
 * background copy-forward runs concurrently with the delete traffic.
 */
static _Atomic int gc_stop;
static _Atomic uint64_t gc_rounds;

static void *gc_forcer(void *unused) {
  (void)unused;
  while (!atomic_load(&gc_stop)) {
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
      }
    }
    atomic_fetch_add(&gc_rounds, 1);
    usleep(200000);
  }
  return NULL;
}

static void *client(void *p) {
  struct targ *a = p;
  pin_me_on(get_nb_workers() + get_nb_distributors() + a->id);

  for (uint64_t i = 0; i < ops_per_thread; i++) {
    /* pick one of this thread's own keys */
    uint64_t slot = rand_r(&a->seed) % (nb_keys / nb_threads);
    uint64_t k = slot * nb_threads + a->id;
    int roll = rand_r(&a->seed) % 100;

    if (roll < delete_pct) { /* delete */
      do_write(k, 0, 2, "mt delete");
      exists[k] = 0;
      do_read(k, 0, 0, "mt read-after-delete");
    } else if (roll < 80) { /* upsert -- writes are always 80% of the mix */
      uint64_t v = (uint64_t)rand_r(&a->seed) << 20 | (k & 0xfffff);
      do_write(k, v, 1, "mt upsert");
      exists[k] = 1;
      values[k] = v;
      do_read(k, 1, v, "mt read-after-upsert");
    } else { /* plain read */
      do_read(k, exists[k], values[k], "mt read");
    }
  }
  return NULL;
}

int main(int argc, char **argv) {
  if (argc > 1) nb_keys = strtoull(argv[1], NULL, 0);
  if (argc > 2) nb_threads = atoi(argv[2]);
  if (argc > 3) ops_per_thread = strtoull(argv[3], NULL, 0);
  nb_keys = (nb_keys / nb_threads) * nb_threads;

  init_default_config(&cfg);
  cfg.kv_size = TEST_KV_SIZE;
  cfg.max_file_size = TEST_MAX_FILE_SIZE;
  cfg.page_cache_size = PAGE_SIZE * 8192;
  cfg.with_reins = getenv("E2E_REINS") ? 1 : 0;
  cfg.with_rebal = getenv("E2E_REBAL") ? 1 : 0;
  cfg.nb_items_in_db = nb_keys;
  cfg.epoch = 1000; /* make reinsertion actually trigger */
  if (getenv("E2E_DELETE_PCT")) delete_pct = atoi(getenv("E2E_DELETE_PCT"));
  if (getenv("E2E_EPOCH")) cfg.epoch = strtoul(getenv("E2E_EPOCH"), NULL, 0);

  printf("== concurrent DELETE (%lu keys, %d threads, %lu ops/thread, "
         "reins=%d rebal=%d) ==\n",
         nb_keys, nb_threads, ops_per_thread, cfg.with_reins, cfg.with_rebal);
  printf("   delete_pct=%d\n", delete_pct);

  exists = calloc(nb_keys, 1);
  values = calloc(nb_keys, sizeof(*values));

  slab_workers_init(1, nb_io_workers, nb_distributors);
  if (cfg.with_reins) fsst_worker_init();

  for (uint64_t k = 0; k < nb_keys; k++) {
    do_write(k, k * 7 + 1, 0, "load");
    exists[k] = 1;
    values[k] = k * 7 + 1;
  }
  printf("  loaded %lu keys\n", nb_keys);

  if (cfg.with_rebal) tnt_rebalancing();

  pthread_t gc_t;
  int force_gc = cfg.with_reins && getenv("E2E_FORCE_GC") != NULL;
  if (force_gc) pthread_create(&gc_t, NULL, gc_forcer, NULL);

  pthread_t *t = calloc(nb_threads, sizeof(*t));
  struct targ *args = calloc(nb_threads, sizeof(*args));
  for (int i = 0; i < nb_threads; i++) {
    args[i].id = i;
    args[i].seed = 0x5eed + i * 7919;
    pthread_create(&t[i], NULL, client, &args[i]);
  }
  for (int i = 0; i < nb_threads; i++) pthread_join(t[i], NULL);
  if (force_gc) {
    atomic_store(&gc_stop, 1);
    pthread_join(gc_t, NULL);
    for (int i = 0; i < 60 && !bgq_is_empty(GC); i++) sleep(1);
    sleep(3);
    printf("  forced %lu reinsertion rounds\n", atomic_load(&gc_rounds));
  }
  printf("  mixed phase done (%lu ops)\n", ops_per_thread * nb_threads);

  /* final sweep: every key must match the oracle */
  for (uint64_t k = 0; k < nb_keys; k++)
    do_read(k, exists[k], values[k], "final sweep");
  printf("  final sweep done\n");

  uint64_t f = atomic_load(&failures);
  int shown = atomic_load(&fail_msgs);
  if (shown > FAILN) shown = FAILN;
  for (int i = 0; i < shown; i++) printf("FAIL %s\n", fail_msg[i]);
  if (f) {
    printf("\n== %lu FAILURES (%d shown) ==\n", f, shown);
    return EXIT_FAILURE;
  }
  printf("\n== concurrent DELETE checks passed ==\n");
  return EXIT_SUCCESS;
}
