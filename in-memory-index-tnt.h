#ifndef IN_MEMORY_TNT
#define IN_MEMORY_TNT 1

#include "indexes/tnt_centree.h"
#include "indexes/tnt_subtree.h"

#define INDEX_TYPE "TNT"
#define tnt_tree_lookup centree_worker_lookup
#define tnt_index_add_utree subtree_worker_lookup_utree
#define tnt_index_lookup_utree subtree_worker_lookup_utree
#define tnt_index_invalid_utree subtree_worker_invalid_utree
#define tnt_index_delete subtree_worker_delete

enum fsst_mode { GC, FSST };

tree_entry_t *centree_worker_lookup(void *key);

int subtree_worker_invalid_utree(subtree_t *tree, void *item);
index_entry_t *subtree_worker_lookup_utree(subtree_t *tree, void *item);
index_entry_t *subtree_worker_lookup_ukey(subtree_t *tree, uint64_t key);
int subtree_worker_delete(subtree_t *tree, void *item);

void centree_init(void);
/* Split/restructure phase exclusion; these do not guard topology pointers. */
void tnt_split_phase_enter(void);
void tnt_split_phase_exit(void);
struct tree_entry *tnt_worker_lookup(int worker_id, void *item);

int tnt_centree_node_is_child (centree_node n);
centree_node tnt_routing_left(centree_node n);
centree_node tnt_routing_right(centree_node n);
centree_node tnt_routing_parent(centree_node n);
uint64_t tnt_get_centree_level(void *n);
subtree_t *tnt_subtree_create(void);
void wakeup_subtree_get(void *n);
void tnt_subtree_add(struct slab *s, void *tree, void *filter,
                     uint64_t tmp_key);
void tnt_subtree_delete(int worker_id, void *item);
void tnt_subtree_update_key(uint64_t old_key, uint64_t new_key);

tree_entry_t *tnt_parent_subtree_get(void *centnode);
tree_entry_t *tnt_subtree_get(void *key, uint64_t *idx, index_entry_t *old_e);
struct tree_entry* centree_lookup_and_reserve(
  void *item,
  uint64_t *out_idx,
  index_entry_t **out_e);

tree_entry_t *tnt_traverse_use_seq(int seq);

int tnt_get_nodes_at_level(int level, background_queue *q);

void swizzle_by_slab(size_t *arr, size_t nb_items, double x_percent);
void tnt_index_add(struct slab_callback *cb, void *item);
index_entry_t *tnt_index_lookup(struct slab_callback *cb, void *item);
index_entry_t *tnt_index_lookup_for_test(struct slab_callback *cb, void *item, int *ttry, uint64_t *tkey);
int tnt_index_invalid(void *item);

uint64_t tnt_get_depth(void);
uint64_t tnt_get_node_count(void);
bool tnt_rebalancing_needed(void);
void tnt_print(void);

enum tnt_rebalance_status {
  TNT_REBALANCE_SUCCESS = 0,
  TNT_REBALANCE_NOOP = 1,
};

/*
 * Safe while client operations are running. The caller must not hold a slab
 * lock or centree_root_lock. Returns a status above or a negative errno value.
 */
int tnt_rebalancing(void);

/* Test-only hook: runs after RCU preparation and before the commit lock. */
void tnt_set_rebalance_precommit_test_hook(void (*hook)(void));
/* Test-only hook: runs after publication/root unlock and before RCU cleanup. */
void tnt_set_rebalance_postpublish_test_hook(void (*hook)(void));

background_queue *bgq_get(enum fsst_mode m);
int bgq_is_empty(enum fsst_mode m);
int bgq_count(enum fsst_mode m);
void bgq_enqueue(enum fsst_mode m, void *n);
void *bgq_dequeue(enum fsst_mode m);
void *bgq_front(enum fsst_mode m);
void *bgq_front_node(enum fsst_mode m);

centree_node dequeue_specific_node(background_queue *queue,
                                   centree_node target);
tree_entry_t *get_next_node_entry(background_queue *queue, centree_node target);
centree_node get_next_node(background_queue *queue, centree_node target);

#endif
