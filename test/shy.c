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

  if (failures) {
    printf("== %lu failures ==\n", failures);
    return 1;
  }
  printf("== ok ==\n");
  return 0;
}
