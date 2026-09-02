#ifndef SUBTREE_H
#define SUBTREE_H 1

#ifdef __cplusplus
extern "C" {
#endif

#include "memory-item.h"
typedef struct subtree {
  void *slab;
  void *tree;
} subtree_t;

subtree_t *subtree_create();
int subtree_find(subtree_t *t, unsigned char *k, size_t len,
                 struct index_entry *e);
int subtree_set_invalid(subtree_t *t, unsigned char *k, size_t len);
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
int subtree_forall_invalid(subtree_t *t, void *data, void (*cb)(void *slab, uint64_t slab_idx));
int subtree_sample_percent(subtree_t *t,
                           uint64_t *out_keys,
                           size_t sample_cnt);
void subtree_free(subtree_t *t);

#ifdef __cplusplus
}
#endif

#endif
