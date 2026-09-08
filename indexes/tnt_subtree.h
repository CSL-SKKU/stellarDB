#ifndef SUBTREE_H
#define SUBTREE_H 1

#ifdef __cplusplus
extern "C" {
#endif

#include "memory-item.h"
typedef struct subtree {
  void *slab;
  void *tree;
  void *report_tombstones; /* optional slot bitmap; no persisted format change */
  uint64_t live_tombstones;
  uint64_t marked_count; /* invalid-bit population; atomic updates under caller locks */
} subtree_t;

subtree_t *subtree_create();
/* Caller holds the slab write lock, or owns an unpublished subtree. */
void subtree_report_record(subtree_t *t, uint64_t key, uint64_t slot, int tombstone);
void subtree_report_counts(subtree_t *t, uint64_t *total, uint64_t *stale, uint64_t *tombstones);
int subtree_find(subtree_t *t, unsigned char *k, size_t len,
                 struct index_entry *e);
int subtree_set_invalid(subtree_t *t, unsigned char *k, size_t len);
#ifdef STELLAR_TESTING
uint64_t subtree_marked_total(void); /* marked entries in allocated local indexes */
#endif
void subtree_set_slab(subtree_t *t, void *slab);
int subtree_delete(subtree_t *t, unsigned char *k, size_t len);
void subtree_insert(subtree_t *t, unsigned char *k, size_t len,
                    struct index_entry *e);
/* Same, with the shy bit set on the stored slot word. */
void subtree_insert_shy(subtree_t *t, unsigned char *k, size_t len,
                        struct index_entry *e);
/* Clears the shy bit; returns 1 if the key was present. Keeps the other bits. */
int subtree_clear_shy(subtree_t *t, unsigned char *k, size_t len);

int subtree_forall_keys(subtree_t *t, void (*cb)(uint64_t h, int n, void *data),
                         void *data);
/*
 * Every entry, in key order, with the raw slot word: bit 31 is the invalid
 * hint, so the caller decides what stale means.
 */
int subtree_forall_entries(subtree_t *t,
                           void (*cb)(uint64_t key, uint32_t slot, void *data),
                           void *data);
/* Historical preorder; root has parent SIZE_MAX, every other parent precedes
 * its children. The caller owns all indexes exclusively for the entire sweep.
 * No insert/erase/free or topology change may run, including in progress(). */
struct subtree_idle_node {
  subtree_t *index;
  size_t *valid;
  size_t parent;
};
/* Mark all descendant-shadowed entries, including those behind tombstones.
 * Uses only the active ancestor path; no disk I/O or per-key B-tree lookup.
 * A started sweep runs to completion. Returns 0 or a negative errno; partial
 * marking on error is safe and idempotent. progress() may report, not mutate. */
int subtree_invalidate_idle(struct subtree_idle_node *nodes, size_t count,
                            void (*progress)(void *), void *context,
                            uint64_t *invalidated);
int subtree_forall_invalid(subtree_t *t, void *data, void (*cb)(void *slab, uint64_t slab_idx));
void subtree_free(subtree_t *t);

#ifdef __cplusplus
}
#endif

#endif
