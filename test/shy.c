/*
 * Reinsertion series, step 1: the shy flag on a record.
 *
 * A shy record is a copy of an older authoritative record that reinsertion
 * moved nearer the leaf. The flag rides in item_metadata.key_size_flags next
 * to the tombstone flag and must be invisible to every helper that reads the
 * key size, the stored size, or tombstone-ness.
 */
#include "headers.h"

#include <stdarg.h>
#include <stdbool.h>

int print = 0;
int load = 0;
int rc_thr = 1;

/* Recovery's per-slot indexer (slabworker.c); not in a header. */
int add_existing_item(struct slab *s, size_t idx, void *item,
                      struct slab_callback *cb);
/* Read completion entry point (slab.c), exercised without an I/O worker. */
void read_item_async_cb(struct slab_callback *cb);

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

/* ------------------------------------------- the two write completions */

#define T_ITEM 64
#define T_MAX 16

static struct slab *fake_slab(uint64_t seq) {
  struct slab *s = calloc(1, sizeof(*s));
  tree_entry_t value = {0};

  s->seq = seq;
  s->min = UINT64_MAX;
  s->max = 0;
  s->item_size = T_ITEM;
  s->nb_max_items = T_MAX;
  s->size_on_disk = T_MAX * T_ITEM + PAGE_SIZE;
  s->fd = -1;
  s->subtree = tnt_subtree_create();
  subtree_set_slab(s->subtree, s);
  atomic_init(&s->full, 0);
  atomic_init(&s->last_item, 0);
  INIT_LOCK(&s->tree_lock, NULL);
  value.slab = s;
  s->centree_node = centree_node_new((void *)(uintptr_t)seq, &value);
  return s;
}

static unsigned char *fake_item(uint64_t key, uint64_t value) {
  unsigned char *item = calloc(1, T_ITEM);
  struct item_metadata *meta = (struct item_metadata *)item;

  item_init(meta, sizeof(uint64_t), sizeof(uint64_t));
  *(uint64_t *)(item + sizeof(*meta)) = key;
  *(uint64_t *)(item + sizeof(*meta) + sizeof(uint64_t)) = value;
  return item;
}

/* Put key at slot in s as a normal entry. */
static void seed(struct slab *s, uint64_t key, size_t slot) {
  struct slab_callback cb = {.slab = s, .slab_idx = slot};
  unsigned char *item = fake_item(key, 0);

  tnt_index_add(&cb, item);
  slab_widen_range(s, key); /* as add_in_tree() does for a real record */
  s->nb_items++;
  free(item);
}

static void run_reins(struct slab *dst, size_t slot, struct slab *src,
                      size_t src_slot, uint64_t key) {
  struct slab_callback cb = {
      .slab = dst, .slab_idx = slot, .fsst_slab = src, .fsst_idx = src_slot};
  unsigned char *item = fake_item(key, 0);

  dst->update_ref = 1;
  dst->nb_items++; /* the slot was reserved */
  add_in_tree_for_reinsertion(&cb, item);
  free(item);
}

static void run_upsert(struct slab *dst, size_t slot, struct slab *src,
                       size_t src_slot, uint64_t key) {
  struct slab_callback cb = {
      .slab = dst, .slab_idx = slot, .fsst_slab = src, .fsst_idx = src_slot};
  unsigned char *item = fake_item(key, 0);

  dst->update_ref = 1;
  dst->nb_items++;
  cb.item = (char *)item;
  add_in_tree_for_upsert(&cb, item);
  free(item);
}

static int lookup(struct slab *s, uint64_t key, index_entry_t *out) {
  unsigned char *item = fake_item(key, 0);
  index_entry_t *e = tnt_index_lookup_utree(s->subtree, item);

  if (e) *out = *e;
  free(item);
  return e != NULL;
}

static void test_completions(void) {
  struct slab *src = fake_slab(1), *dst = fake_slab(2);
  index_entry_t e;
  const uint64_t K = 777;

  centree_init();
  printf("== completions ==\n");

  /* R publishes: key absent from dst, source valid -> shy entry, source invalidated. */
  seed(src, K, 3);
  run_reins(dst, 10, src, 3, K);
  check(lookup(dst, K, &e) && sidx_is_shy(e.slab_idx) && GET_SIDX(e.slab_idx) == 10,
        "R did not publish a shy entry");
  check(lookup(src, K, &e) && sidx_is_invalid(e.slab_idx),
        "R did not invalidate its source");
  check(dst->nb_items == 1 && src->nb_items == 0, "counts after a published R (%zu/%zu)",
        dst->nb_items, src->nb_items);

  /* W with a *lower* slot completes later: it must replace the shy entry. */
  run_upsert(dst, 9, src, 3, K);
  check(lookup(dst, K, &e) && !sidx_is_shy(e.slab_idx) && GET_SIDX(e.slab_idx) == 9,
        "a later client write did not beat the shy entry (word %zx)", e.slab_idx);
  check(dst->nb_items == 1, "count after W over shy (%zu)", dst->nb_items);

  /* R completes when the key is already in dst: abort, nothing changes. */
  seed(src, K + 1, 4);
  seed(dst, K + 1, 5);
  run_reins(dst, 11, src, 4, K + 1);
  check(lookup(dst, K + 1, &e) && GET_SIDX(e.slab_idx) == 5 && !sidx_is_shy(e.slab_idx),
        "R overrode an existing client entry");
  check(lookup(src, K + 1, &e) && !sidx_is_invalid(e.slab_idx),
        "an aborted R invalidated its source");
  check(dst->nb_items == 2, "aborted R left a count behind (%zu)", dst->nb_items);

  /* R completes after its source was invalidated: abort. */
  seed(src, K + 2, 6);
  {
    unsigned char *item = fake_item(K + 2, 0);
    tnt_index_invalid_utree(src->subtree, item);
    free(item);
  }
  run_reins(dst, 12, src, 6, K + 2);
  check(!lookup(dst, K + 2, &e), "R published from an invalidated source");

  /* R completes after its source was retired: abort. */
  seed(src, K + 3, 7);
  src->min = UINT64_MAX;
  src->max = 0;
  run_reins(dst, 13, src, 7, K + 3);
  check(!lookup(dst, K + 3, &e), "R published from a retired source");
  src->min = 0;
  src->max = UINT64_MAX;

  /* Two client writes: the higher slot still wins, regardless of order. */
  seed(src, K + 4, 8);
  run_upsert(dst, 20, src, 8, K + 4);
  run_upsert(dst, 19, src, 8, K + 4);
  check(lookup(dst, K + 4, &e) && GET_SIDX(e.slab_idx) == 20,
        "client-vs-client tie-break changed (slot %zu)", GET_SIDX(e.slab_idx));

  /* W first, then R with a higher slot: R must abort, W stays. */
  seed(src, K + 5, 9);
  run_upsert(dst, 21, src, 9, K + 5);
  run_reins(dst, 22, src, 9, K + 5);
  check(lookup(dst, K + 5, &e) && GET_SIDX(e.slab_idx) == 21 && !sidx_is_shy(e.slab_idx),
        "R with a higher slot beat a client write");
}

/* ------------------------------------------- recovery over shy records */

static void feed(struct slab *s, size_t slot, uint64_t key, int shy, int expect) {
  struct slab_callback cb = {.slab = s};
  unsigned char *item = fake_item(key, slot);

  if (shy) item_mark_shy((struct item_metadata *)item);
  check(add_existing_item(s, slot, item, &cb) == expect,
        "add_existing_item(slot %zu, %s) return value", slot, shy ? "shy" : "normal");
  free(item);
}

static void expect_slot(struct slab *s, uint64_t key, size_t slot, int shy,
                        const char *what) {
  index_entry_t e;

  if (!lookup(s, key, &e)) {
    check(false, "%s: key %lu not indexed", what, key);
    return;
  }
  check(GET_SIDX(e.slab_idx) == slot && !!sidx_is_shy(e.slab_idx) == !!shy,
        "%s: key %lu indexed at slot %zu%s, want %zu%s", what, key,
        GET_SIDX(e.slab_idx), sidx_is_shy(e.slab_idx) ? " (shy)" : "", slot,
        shy ? " (shy)" : "");
}

static void test_recovery_rule(void) {
  struct slab *s = fake_slab(9);

  printf("== recovery tie-break ==\n");
  /* normal then shy: the client record stays, the shy one is skipped */
  feed(s, 1, 100, 0, 1);
  feed(s, 2, 100, 1, 1);
  expect_slot(s, 100, 1, 0, "normal then shy");
  /* shy then normal: the client record replaces the copy */
  feed(s, 3, 101, 1, 1);
  feed(s, 4, 101, 0, 1);
  expect_slot(s, 101, 4, 0, "shy then normal");
  /* shy then shy: later wins, stays shy */
  feed(s, 5, 102, 1, 1);
  feed(s, 6, 102, 1, 1);
  expect_slot(s, 102, 6, 1, "shy then shy");
  /* normal then normal: later wins, as before */
  feed(s, 7, 103, 0, 1);
  feed(s, 8, 103, 0, 1);
  expect_slot(s, 103, 8, 0, "normal then normal");
  /* a lone shy record is indexed, and indexed as shy */
  feed(s, 9, 104, 1, 1);
  expect_slot(s, 104, 9, 1, "lone shy");
  check(s->nb_items == 5, "nb_items after the tie-breaks is %zu, want 5",
        s->nb_items);
}

/* ------------------------------------------------------ shy tombstones */

/* A tombstone as the reinsertion worker copies it: both bits set. */
static unsigned char *fake_shy_tombstone(uint64_t key) {
  unsigned char *item = fake_item(key, 0);
  struct item_metadata *meta = (struct item_metadata *)item;

  item_encode_tombstone(meta);
  item_mark_shy(meta);
  return item;
}

static void feed_tomb(struct slab *s, size_t slot, uint64_t key, int expect) {
  struct slab_callback cb = {.slab = s};
  unsigned char *item = fake_shy_tombstone(key);

  check(add_existing_item(s, slot, item, &cb) == expect,
        "add_existing_item(slot %zu, shy tombstone) return value", slot);
  free(item);
}

static void test_shy_tombstone(void) {
  struct slab *src = fake_slab(11), *dst = fake_slab(12), *rec = fake_slab(13);
  unsigned char *item = fake_shy_tombstone(500);
  struct item_metadata *meta = (struct item_metadata *)item;
  struct slab_callback cb = {
      .slab = dst, .slab_idx = 4, .fsst_slab = src, .fsst_idx = 2};
  index_entry_t e;

  printf("== shy tombstone ==\n");

  /* The record keeps both bits and its partial-slot size. */
  check(item_is_tombstone(meta) && item_is_shy(meta),
        "shy tombstone lost a bit (%zx)", meta->key_size_flags);
  check(item_stored_size(meta) == sizeof(*meta) + sizeof(uint64_t),
        "shy tombstone stored size is %zu", item_stored_size(meta));

  /* Copy-forward of a tombstone: published shy, source invalidated. */
  seed(src, 500, 2);
  dst->update_ref = 1;
  dst->nb_items++;
  cb.item = (char *)item;
  add_in_tree_for_reinsertion(&cb, item);
  check(lookup(dst, 500, &e) && sidx_is_shy(e.slab_idx) && GET_SIDX(e.slab_idx) == 4,
        "shy tombstone was not published");
  check(lookup(src, 500, &e) && sidx_is_invalid(e.slab_idx),
        "shy tombstone did not invalidate its source");

  /* A client resurrecting the key later, in a lower slot, still wins. */
  run_upsert(dst, 3, src, 2, 500);
  check(lookup(dst, 500, &e) && !sidx_is_shy(e.slab_idx) && GET_SIDX(e.slab_idx) == 3,
        "client write did not beat the shy tombstone");
  free(item);

  /* Recovery: a shy tombstone yields to a client record either way round. */
  feed(rec, 1, 600, 0, 1);
  feed_tomb(rec, 2, 600, 1);
  expect_slot(rec, 600, 1, 0, "client record then shy tombstone");
  feed_tomb(rec, 3, 601, 1);
  feed(rec, 4, 601, 0, 1);
  expect_slot(rec, 601, 4, 0, "shy tombstone then client record");
  /* Alone it is indexed, as shy, so the key reads as deleted. */
  feed_tomb(rec, 5, 602, 1);
  expect_slot(rec, 602, 5, 1, "lone shy tombstone");
  check(rec->nb_items == 3, "nb_items after shy tombstones is %zu, want 3",
        rec->nb_items);
}

/* Exercise the real read-completion gate without submitting disk writes.
 * The source is deliberately unindexed, so qualifying attempts fail their
 * authority check after updating the trigger counters. */
static void test_on_read_trigger(void) {
  struct slab *src = fake_slab(700);
  unsigned char *item = fake_item(700, 1);
  struct lru page = {.page = (char *)item};
  struct slab_callback cb = {
      .slab = src, .slab_idx = 0, .lru_entry = &page,
      .action = READ_NO_LOOKUP, .upward_len = 6, .page_was_hot = 1};

  init_default_config(&cfg);
  check(!cfg.with_reins && cfg.reins_multiplier == 1.0 && cfg.reins_sample == 1,
        "reinsertion defaults changed");
  centree_init();
  /* Control advisory metadata: ceil(log2(31+1)) = 5, regardless of depth.
   * Routing stays empty because every attempted copy must fail authority. */
  atomic_store(&tnt_centree()->node_count, 31);
  atomic_store(&tnt_centree()->depth, 1000);
  reset_restructuring_stats();

  src->read_ref++;
  read_item_async_cb(&cb);
  check(rstats.reins_or_seen == 0, "a read triggered reinsertion without -r");

  cfg.with_reins = 1;
  cb.page_was_hot = 0;
  src->read_ref++;
  read_item_async_cb(&cb);
  check(rstats.reins_or_seen == 1 && rstats.reins_or_deep == 0,
        "a cold page qualified");

  cb.page_was_hot = 1;
  cb.upward_len = 5; /* four parent hops: just below the threshold */
  src->read_ref++;
  read_item_async_cb(&cb);
  check(rstats.reins_or_deep == 0, "a short history walk qualified");

  cb.upward_len = 6; /* five parent hops: exactly at the threshold */
  src->read_ref++;
  read_item_async_cb(&cb);
  check(rstats.reins_or_deep == 1 && rstats.reins_examined == 1,
        "the logarithmic boundary did not qualify on a skewed routing tree");

  atomic_store(&tnt_centree()->depth, 5);
  src->read_ref++;
  read_item_async_cb(&cb);
  check(rstats.reins_or_deep == 2 && rstats.reins_examined == 2,
        "routing depth changed the trigger");

  atomic_store(&tnt_centree()->node_count, 33); /* ideal depth grows to six */
  src->read_ref++;
  read_item_async_cb(&cb);
  check(rstats.reins_or_deep == 2, "node count did not raise the threshold");

  const struct {
    uint64_t nodes;
    double multiplier;
    int upward_len, qualifies;
  } cases[] = {
      {31, 0.5, 3, 0},  /* ceil(0.5 * log2(32)) = 3 hops */
      {31, 0.5, 4, 1},
      {31, 2.0, 10, 0}, /* ceil(2 * log2(32)) = 10 hops */
      {31, 2.0, 11, 1},
      {33, 1.1, 6, 0},  /* multiply before rounding: ceil(1.1 * log2(34)) = 6 */
      {33, 1.1, 7, 1},
      {31, 0.0, 1, 1},  /* zero bypasses distance, including a leaf hit */
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    uint64_t deep_before = rstats.reins_or_deep;
    uint64_t examined_before = rstats.reins_examined;

    atomic_store(&tnt_centree()->node_count, cases[i].nodes);
    cfg.reins_multiplier = cases[i].multiplier;
    cb.upward_len = cases[i].upward_len;
    src->read_ref++;
    read_item_async_cb(&cb);
    check(rstats.reins_or_deep == deep_before + cases[i].qualifies &&
              rstats.reins_examined == examined_before + cases[i].qualifies,
          "multiplier %.2f, %lu nodes, upward_len %d: wrong trigger result",
          cases[i].multiplier, cases[i].nodes, cases[i].upward_len);
  }
  cfg.reins_multiplier = 1.0;
  check(rstats.reins_issued == 0 && rstats.reins_published == 0 && src->read_ref == 0,
        "an unindexed source was copied or leaked a read reference");
  subtree_free(src->subtree);
  pthread_rwlock_destroy(&src->tree_lock);
  free(src->centree_node);
  free(src);
  free(item);
  puts("  on-read enable, page reuse and scaled logarithmic history threshold passed");
}

int main(void) {
  struct item_metadata normal, tomb, empty = {0}, legacy;

  printf("== shy flag ==\n");

  item_init(&normal, 8, 100);
  check(!item_is_shy(&normal), "a fresh record is shy");
  item_mark_shy(&normal);
  check(item_is_shy(&normal), "mark did not stick");
  check(item_key_size(&normal) == 8, "shy changed the key size (%zu)",
        item_key_size(&normal));
  check(item_stored_size(&normal) == sizeof(normal) + 8 + 100,
        "shy changed the stored size");
  check(!item_is_tombstone(&normal), "shy looks like a tombstone");
  check(!item_is_empty(&normal), "shy looks empty");
  check(!item_is_legacy(&normal), "shy looks legacy");
  item_clear_shy(&normal);
  check(!item_is_shy(&normal), "clear did not clear");
  check(normal.key_size_flags == 8, "clear left other bits (%zx)",
        normal.key_size_flags);

  /* A reinserted tombstone carries both bits. */
  item_init_tombstone(&tomb, 8);
  item_mark_shy(&tomb);
  check(item_is_tombstone(&tomb) && item_is_shy(&tomb),
        "tombstone and shy do not coexist");
  check(item_stored_size(&tomb) == sizeof(tomb) + 8,
        "shy tombstone stored size changed");
  item_clear_shy(&tomb);
  check(item_is_tombstone(&tomb) && !item_is_shy(&tomb),
        "clearing shy touched the tombstone bit");

  /* Encoding a tombstone requires a clean flag word: stripping first works. */
  item_init(&normal, 8, 100);
  item_mark_shy(&normal);
  item_clear_shy(&normal);
  item_encode_tombstone(&normal);
  check(item_is_tombstone(&normal) && !item_is_shy(&normal),
        "tombstone encoding after strip is wrong");

  check(!item_is_shy(&empty), "an empty slot is shy");
  legacy.key_size_flags = SIZE_MAX;
  check(!item_is_shy(&legacy), "a legacy record is shy");
  item_clear_shy(&legacy);
  check(item_is_legacy(&legacy), "clear touched a legacy record");

  /* ---- the slot word in the local index ---- */
  {
    subtree_t *t = subtree_create();
    struct slab fake = {0};
    uint64_t k1 = 11, k2 = 22;
    index_entry_t e = {{&fake}, {5}}, found;

    subtree_set_slab(t, &fake);
    subtree_insert(t, (unsigned char *)&k1, sizeof(k1), &e);
    e.slab_idx = 6;
    subtree_insert_shy(t, (unsigned char *)&k2, sizeof(k2), &e);

    check(subtree_find(t, (unsigned char *)&k1, sizeof(k1), &found) &&
              !sidx_is_shy(found.slab_idx) && GET_SIDX(found.slab_idx) == 5,
          "normal entry is wrong");
    check(subtree_find(t, (unsigned char *)&k2, sizeof(k2), &found) &&
              sidx_is_shy(found.slab_idx) && GET_SIDX(found.slab_idx) == 6,
          "shy entry is wrong (word %zx)", found.slab_idx);
    check(!sidx_is_invalid(found.slab_idx), "shy entry reads as invalid");

    /* The invalid hint and the shy bit are independent. */
    subtree_set_invalid(t, (unsigned char *)&k2, sizeof(k2));
    subtree_find(t, (unsigned char *)&k2, sizeof(k2), &found);
    check(sidx_is_invalid(found.slab_idx) && sidx_is_shy(found.slab_idx) &&
              GET_SIDX(found.slab_idx) == 6,
          "invalidating a shy entry lost a bit (word %zx)", found.slab_idx);
    check(subtree_clear_shy(t, (unsigned char *)&k2, sizeof(k2)) == 1,
          "clear_shy did not find the key");
    subtree_find(t, (unsigned char *)&k2, sizeof(k2), &found);
    check(sidx_is_invalid(found.slab_idx) && !sidx_is_shy(found.slab_idx) &&
              GET_SIDX(found.slab_idx) == 6,
          "clear_shy touched other bits (word %zx)", found.slab_idx);
    check(subtree_clear_shy(t, (unsigned char *)&k1, sizeof(k1)) == 1 &&
              subtree_find(t, (unsigned char *)&k1, sizeof(k1), &found) &&
              found.slab_idx == 5,
          "clear_shy on a normal entry changed it");
    uint64_t missing = 33;
    check(subtree_clear_shy(t, (unsigned char *)&missing, sizeof(missing)) == 0,
          "clear_shy invented a key");
    subtree_free(t);
  }

  test_completions();
  test_recovery_rule();
  test_shy_tombstone();
  test_on_read_trigger();

  if (failures) {
    printf("== %lu failures ==\n", failures);
    return 1;
  }
  printf("== ok ==\n");
  return 0;
}
