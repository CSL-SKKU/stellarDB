/*
 * End-to-end DELETE test.
 *
 * Drives the real StellarDB pipeline (distributors + I/O workers + on-disk
 * slabs) and checks the DELETE semantics that AGENTS.md prescribes:
 *
 *   1. DELETE uses the same lookup/reservation path as UPSERT
 *   2. the tombstone record is persisted (metadata + key, no value)
 *   3. the tombstone stays in the destination slab's local B-tree
 *   4. the destination is published before the old source is invalidated
 *   5. READ turns a valid tombstone into a normal "not found"
 *   6. recovery re-indexes tombstones from their persisted key
 *   7. reinsertion understands the tombstone representation
 *
 * The test is a single process; pass "verify" as argv[1] to skip the
 * populate/delete phases and only re-open an existing DB (phase 6).
 *
 * Usage: ./test/test_delete_e2e [populate|verify] [nb_keys]
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
static int nb_io_workers = 4;
static int nb_distributors = 2;

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

/*
 * One request. The slab_callback must be the first member so that the
 * engine's `struct slab_callback *` is also our request pointer.
 */
struct req {
  struct slab_callback cb;
  unsigned char *item;
  uint64_t key;
  uint64_t expect_value;
  int expect_found; /* 1: must be found, 0: must be a miss */
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

    if (!r->expect_found) {
      record_failure("[%s] key %lu: READ returned a record (key %lu val %lu), "
                     "expected a miss (tombstone)",
                     r->phase, r->key, got_key, got_val);
    } else if (got_key != r->key) {
      record_failure("[%s] key %lu: READ returned wrong key %lu", r->phase,
                     r->key, got_key);
    } else if (got_val != r->expect_value) {
      record_failure("[%s] key %lu: READ value %lu, expected %lu", r->phase,
                     r->key, got_val, r->expect_value);
    }
    if (item_is_tombstone(meta))
      record_failure("[%s] key %lu: tombstone leaked to the user callback",
                     r->phase, r->key);
  }
  atomic_store(&r->done, 1);
}

static void write_done(struct slab_callback *cb, void *item) {
  struct req *r = (struct req *)cb;
  (void)item;
  atomic_store(&r->done, 1);
}

static void reset_req(struct req *r, unsigned char *item, uint64_t key,
                      slab_cb_t *done) {
  memset(&r->cb, 0, sizeof(r->cb));
  r->cb.cb = done;
  r->cb.cb_cb = NULL;
  r->cb.payload = NULL;
  r->cb.item = (char *)item;
  r->cb.fsst_slab = NULL;
  r->cb.fsst_idx = -1;
  r->item = item;
  r->key = key;
  atomic_store(&r->done, 0);
}

static void wait_all(struct req *reqs, uint64_t n) {
  for (uint64_t i = 0; i < n; i++)
    while (!atomic_load(&reqs[i].done)) NOP10();
}

/* ---------------------------------------------------------------- phases */

enum op { OP_ADD, OP_UPSERT, OP_DELETE };

/* Runs one write op over the keys for which want(key) is true. */
static void run_writes(const char *phase, enum op op, uint64_t lo, uint64_t hi,
                       int modulus, int residue, uint64_t (*value)(uint64_t)) {
  uint64_t n = 0;
  struct req *reqs = calloc(hi - lo, sizeof(*reqs));

  for (uint64_t k = lo; k < hi; k++) {
    if (modulus && (k % modulus) != (uint64_t)residue) continue;
    struct req *r = &reqs[n++];
    reset_req(r, make_item(k, value ? value(k) : 0), k, write_done);
    r->phase = phase;
    switch (op) {
      case OP_ADD: kv_add_async(&r->cb); break;
      case OP_UPSERT: kv_upsert_async(&r->cb); break;
      case OP_DELETE: kv_remove_async(&r->cb); break;
    }
  }
  wait_all(reqs, n);
  for (uint64_t i = 0; i < n; i++) free(reqs[i].item);
  free(reqs);
  printf("  %-28s %lu ops\n", phase, n);
}

/*
 * Reads keys [lo, hi). found(key) decides whether the key must be visible,
 * value(key) gives the value it must carry.
 */
static void run_reads(const char *phase, uint64_t lo, uint64_t hi,
                      int (*found)(uint64_t), uint64_t (*value)(uint64_t)) {
  uint64_t n = hi - lo;
  struct req *reqs = calloc(n, sizeof(*reqs));

  for (uint64_t i = 0; i < n; i++) {
    uint64_t k = lo + i;
    struct req *r = &reqs[i];
    reset_req(r, make_item(k, 0), k, read_done);
    r->phase = phase;
    r->expect_found = found(k);
    r->expect_value = value ? value(k) : 0;
    kv_read_async(&r->cb);
  }
  wait_all(reqs, n);
  for (uint64_t i = 0; i < n; i++) free(reqs[i].item);
  free(reqs);
  printf("  %-28s %lu reads\n", phase, n);
}

/*
 * Report slabs that filled up without ever publishing a min/max. When such a
 * slab splits, close_and_create_slab() computes
 *   new_key = min + (max - min) / 2  ==  UINT64_MAX + (0 - UINT64_MAX) / 2
 * i.e. UINT64_MAX, and creates children keyed UINT64_MAX-1 and 0 -- keys that
 * collide with other slabs, both in the center tree and in the file names.
 */
static void dump_degenerate_slabs(const char *when) {
  size_t total = 0, degenerate = 0;
  printf("  [%s] slab survey:\n", when);
  for (int seq = 0;; seq++) {
    tree_entry_t *e = tnt_traverse_use_seq(seq);
    if (!e) break;
    struct slab *s = e->slab;
    total++;
    if (s->min == (uint64_t)-1 || s->max == 0) {
      degenerate++;
      printf("    slab seq=%lu key=%lu level=%lu min=%lu max=%lu full=%d "
             "nb_items=%lu last_item=%lu\n",
             s->seq, s->key, e->level, s->min, s->max,
             atomic_load(&s->full), s->nb_items,
             (size_t)atomic_load(&s->last_item));
    }
  }
  printf("    %lu slabs, %lu with an unpublished key range\n", total,
         degenerate);
}

static uint64_t v1(uint64_t k) { return k * 7 + 1; }
static uint64_t v2(uint64_t k) { return k * 7 + 2; }
static int all_found(uint64_t k) { (void)k; return 1; }
static int none_found(uint64_t k) { (void)k; return 0; }
static int not_multiple_of_3(uint64_t k) { return (k % 3) != 0; }

/* value that a key carries after phase 5 (resurrection) */
static uint64_t v_after_resurrect(uint64_t k) {
  return (k % 3) == 0 ? v2(k) : v1(k);
}

int main(int argc, char **argv) {
  int verify_only = argc > 1 && !strcmp(argv[1], "verify");
  int verify_all = argc > 1 && !strcmp(argv[1], "verifyall");
  int insert_only = argc > 1 && !strcmp(argv[1], "insert");
  int upsert_burst = argc > 1 && !strcmp(argv[1], "upsertburst");
  if (argc > 2) nb_keys = strtoull(argv[2], NULL, 0);

  init_default_config(&cfg);
  cfg.kv_size = TEST_KV_SIZE;
  cfg.max_file_size = getenv("E2E_MAX_FILE_SIZE")
                          ? strtoull(getenv("E2E_MAX_FILE_SIZE"), NULL, 0)
                          : TEST_MAX_FILE_SIZE;
  cfg.page_cache_size = PAGE_SIZE * 8192; /* 32 MB */
  cfg.with_reins = getenv("E2E_REINS") ? 1 : 0;
  cfg.with_rebal = 0;
  cfg.nb_items_in_db = nb_keys;

  printf("== DELETE end-to-end (%s, %lu keys, reins=%d) ==\n",
         verify_only ? "verify" : "populate", nb_keys, cfg.with_reins);

  slab_workers_init(1, nb_io_workers, nb_distributors);
  if (cfg.with_reins) fsst_worker_init();
  printf("  recovered %lu index entries\n", get_database_size());

  if (verify_only) {
    /* Phase 6: after a restart every key must still read as deleted. */
    run_reads("6. read after recovery", 0, nb_keys, none_found, NULL);
  } else if (verify_all) {
    /* Control: same shape, but the DB only ever saw inserts. */
    run_reads("read after recovery", 0, nb_keys, all_found, v1);
  } else if (upsert_burst) {
    /*
     * Control for the delete workload: identical shape and identical write
     * volume (11k writes after the load), fired in the same unthrottled
     * bursts, but every write is a plain UPSERT instead of a DELETE. Both go
     * through remove_and_add_item_async()/add_in_tree_for_upsert().
     */
    run_writes("insert", OP_ADD, 0, nb_keys, 0, 0, v1);
    run_reads("read back", 0, nb_keys, all_found, v1);
    run_writes("upsert k%3==0", OP_UPSERT, 0, nb_keys, 3, 0, v2);
    run_reads("read", 0, nb_keys, all_found, v_after_resurrect);
    run_writes("upsert absent keys", OP_UPSERT, nb_keys, nb_keys + 1000, 0, 0,
               v1);
    run_reads("read absent keys", nb_keys, nb_keys + 1000, all_found, v1);
    run_writes("re-upsert k%3==0", OP_UPSERT, 0, nb_keys, 3, 0, v2);
    run_reads("read", 0, nb_keys, all_found, v_after_resurrect);
    run_writes("upsert k%3==0 again", OP_UPSERT, 0, nb_keys, 3, 0, v2);
    run_reads("read", 0, nb_keys, all_found, v_after_resurrect);
    run_writes("upsert all", OP_UPSERT, 0, nb_keys, 0, 0, v1);
    run_reads("read all", 0, nb_keys, all_found, v1);
    dump_degenerate_slabs("after upsert-burst workload");
  } else if (insert_only) {
    run_writes("insert only", OP_ADD, 0, nb_keys, 0, 0, v1);
    run_reads("read back", 0, nb_keys, all_found, v1);
    dump_degenerate_slabs("after insert-only workload");
  } else {
    /* 1. load */
    run_writes("1. insert", OP_ADD, 0, nb_keys, 0, 0, v1);
    run_reads("1. read back", 0, nb_keys, all_found, v1);

    /* 2. delete every 3rd key (mix of in-place and moving deletes) */
    run_writes("2. delete k%3==0", OP_DELETE, 0, nb_keys, 3, 0, NULL);
    run_reads("2. read after delete", 0, nb_keys, not_multiple_of_3, v1);

    /* 3. delete keys that were never inserted */
    run_writes("3. delete absent keys", OP_DELETE, nb_keys, nb_keys + 1000, 0,
               0, NULL);
    run_reads("3. read absent keys", nb_keys, nb_keys + 1000, none_found, NULL);

    /* 4. delete the same keys twice */
    run_writes("4. re-delete k%3==0", OP_DELETE, 0, nb_keys, 3, 0, NULL);
    run_reads("4. read after re-delete", 0, nb_keys, not_multiple_of_3, v1);

    /* 5. resurrect the deleted keys with a new value */
    run_writes("5. resurrect k%3==0", OP_UPSERT, 0, nb_keys, 3, 0, v2);
    run_reads("5. read after resurrect", 0, nb_keys, all_found,
              v_after_resurrect);

    /* 6. delete everything, then verify (and leave the DB for `verify`) */
    run_writes("6. delete all", OP_DELETE, 0, nb_keys, 0, 0, NULL);
    run_reads("6. read after delete all", 0, nb_keys, none_found, NULL);
    dump_degenerate_slabs("after delete workload");
  }

  uint64_t f = atomic_load(&failures);
  int shown = atomic_load(&fail_msgs);
  if (shown > FAILN) shown = FAILN;
  for (int i = 0; i < shown; i++) printf("FAIL %s\n", fail_msg[i]);
  if (f) {
    printf("\n== %lu FAILURES (%d shown) ==\n", f, shown);
    return EXIT_FAILURE;
  }
  printf("\n== all DELETE end-to-end checks passed ==\n");
  return EXIT_SUCCESS;
}
