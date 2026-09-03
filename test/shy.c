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

  if (failures) {
    printf("== %lu failures ==\n", failures);
    return 1;
  }
  printf("== ok ==\n");
  return 0;
}
