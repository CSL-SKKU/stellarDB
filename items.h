#ifndef ITEMS_H
#define ITEMS_H

#include "headers.h"
#include <limits.h>
#include <stdbool.h>

#define ITEM_TOMBSTONE_FLAG \
  ((size_t)1 << (sizeof(size_t) * CHAR_BIT - 1))
/*
 * A record written by reinsertion: a copy of an older, authoritative record
 * moved nearer the leaf. "Shy" because it yields to any client write of the
 * same key regardless of slot order, in the write completion and in recovery.
 * A client write never carries it (the entry points strip it).
 */
#define ITEM_SHY_FLAG \
  ((size_t)1 << (sizeof(size_t) * CHAR_BIT - 2))
#define ITEM_KEY_SIZE_MASK (ITEM_SHY_FLAG - 1)

struct item_metadata {
  size_t rdt;
  /* Key size plus flags; access only through the item_* helpers below. */
  size_t key_size_flags;
  size_t value_size;
  // key
  // value
};

/* Legacy key_size == SIZE_MAX records are invalid KVell artifacts. */
static inline bool item_is_legacy(const struct item_metadata *meta) {
  return meta->key_size_flags == SIZE_MAX;
}

static inline bool item_is_empty(const struct item_metadata *meta) {
  return meta->key_size_flags == 0;
}

static inline size_t item_key_size(const struct item_metadata *meta) {
  return meta->key_size_flags & ITEM_KEY_SIZE_MASK;
}

static inline size_t item_is_tombstone(const struct item_metadata *meta) {
  return !item_is_legacy(meta) &&
         (meta->key_size_flags & ITEM_TOMBSTONE_FLAG);
}

static inline bool item_is_shy(const struct item_metadata *meta) {
  return !item_is_legacy(meta) && (meta->key_size_flags & ITEM_SHY_FLAG);
}

static inline void item_mark_shy(struct item_metadata *meta) {
  assert(!item_is_legacy(meta) && !item_is_empty(meta));
  meta->key_size_flags |= ITEM_SHY_FLAG;
}

static inline void item_clear_shy(struct item_metadata *meta) {
  if (!item_is_legacy(meta))
    meta->key_size_flags &= ~ITEM_SHY_FLAG;
}

static inline void item_init(struct item_metadata *meta, size_t key_size,
                             size_t value_size) {
  assert(key_size <= ITEM_KEY_SIZE_MASK);
  meta->key_size_flags = key_size;
  meta->value_size = value_size;
}

static inline void item_init_tombstone(struct item_metadata *meta,
                                       size_t key_size) {
  assert(key_size <= ITEM_KEY_SIZE_MASK);
  meta->key_size_flags = key_size | ITEM_TOMBSTONE_FLAG;
  meta->value_size = 0;
}

static inline void item_encode_tombstone(struct item_metadata *meta) {
  assert(meta->key_size_flags <= ITEM_KEY_SIZE_MASK);
  meta->key_size_flags |= ITEM_TOMBSTONE_FLAG;
  meta->value_size = 0;
}

static inline size_t item_stored_size(const struct item_metadata *meta) {
  assert(!item_is_legacy(meta));
  return sizeof(*meta) + item_key_size(meta) +
         (item_is_tombstone(meta) ? 0 : meta->value_size);
}

#endif
