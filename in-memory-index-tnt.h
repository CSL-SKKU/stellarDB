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
/* The one center tree. NULL before centree_init(). */
centree tnt_centree(void);

/*
 * Recovery: given every slab found on disk with its header, build the history
 * tree from the headers' children, derive lu_parent, and build the routing
 * tree over the history in-order sequence. reachable[i] is set for slabs that
 * belong to the tree rooted at root_id; the others are garbage for the caller
 * to delete. Returns the number of reachable slabs, or dies on a corrupt
 * graph (dangling child, two parents, non-alternating in-order).
 */
struct slab_header;
size_t tnt_recover_tree(struct slab **slabs, const struct slab_header *hdrs,
                        size_t n, uint64_t root_id, unsigned char *reachable);
/* Publication lock; see the comment on the definition. */
void tnt_root_wlock(void);
void tnt_root_wunlock(void);
/*
 * Mutual exclusion between the maintenance operations that restructure the
 * center tree (rebalancing, and pruning once it lands). tnt_rebalancing()
 * takes it internally. Lock order:
 *   maintenance_lock -> CENTREE_RESTRUCTURING -> RCU writer -> centree_root_lock
 */
void tnt_maintenance_lock(void);
void tnt_maintenance_unlock(void);
/* Split/restructure phase exclusion; these do not guard topology pointers. */
void tnt_split_phase_enter(void);
void tnt_split_phase_exit(void);
struct tree_entry *tnt_worker_lookup(int worker_id, void *item);

/*
 * Raw center-tree pointers returned below remain memory-valid only because
 * live node/slab reclamation is currently prohibited; they do not pin an RCU
 * routing generation.
 */
int tnt_centree_node_is_child (centree_node n);
centree_node tnt_routing_left(centree_node n);
centree_node tnt_routing_right(centree_node n);
centree_node tnt_routing_parent(centree_node n);
uint64_t tnt_get_centree_level(void *n);
subtree_t *tnt_subtree_create(void);
void wakeup_subtree_get(void *n);
/* The caller must not hold centree_root_lock or a slab lock. */
void tnt_subtree_add(struct slab *s, void *tree, void *filter,
                     uint64_t tmp_key);
/*
 * The caller must already hold one split-phase reference and must not hold
 * centree_root_lock or a slab lock.
 */
void tnt_subtree_add_split(struct slab *s, void *tree, void *filter,
                           uint64_t tmp_key);
void tnt_subtree_delete(int worker_id, void *item);

tree_entry_t *tnt_parent_subtree_get(void *centnode);
tree_entry_t *tnt_subtree_get(void *key, uint64_t *idx, index_entry_t *old_e);
/*
 * Reserve one slot, or (size_t)-1 when the slab is full. Interlocks with
 * slab_freeze() through last_item.
 */
size_t reserve_slot(struct slab *s);
struct tree_entry* centree_lookup_and_reserve(
  void *item,
  uint64_t *out_idx,
  index_entry_t **out_e);

tree_entry_t *tnt_traverse_use_seq(int seq);

int tnt_get_nodes_at_level(int level, background_queue *q);

void swizzle_by_slab(size_t *arr, size_t nb_items, double x_percent);
void tnt_index_add(struct slab_callback *cb, void *item);
void tnt_index_add_shy(struct slab_callback *cb, void *item);
/*
 * On success the returned entry carries one read reference on its slab, taken
 * under the slab lock that found it. The read path passes it to the read
 * completion; every other caller must release it with
 * tnt_index_lookup_unref().
 */
index_entry_t *tnt_index_lookup(struct slab_callback *cb, void *item);
void tnt_index_lookup_unref(index_entry_t *e);
index_entry_t *tnt_index_lookup_for_test(struct slab_callback *cb, void *item, int *ttry, uint64_t *tkey);
int tnt_index_invalid(void *item);

uint64_t tnt_get_depth(void);
uint64_t tnt_get_node_count(void);
bool tnt_rebalancing_needed(void);
void prune_scan_report(const char *phase);
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
/*
 * Test-only hook: runs once per upward-walk step in tnt_index_lookup(),
 * before the node's slab lock is taken and its removed flag is read.
 */
void tnt_set_index_lookup_step_test_hook(void (*hook)(centree_node n));

/*
 * Pruning (indexes/tnt_prune.c). A candidate is an internal-leaf-internal
 * triple that is consecutive in-order and adjacent in the history chain:
 *
 *   leaf -> inner -> outer -> up, with inner->lu_child[side] == leaf and
 *   outer->lu_child[!side] == inner
 *
 * sib and star are the triple's two external history children, i.e. the only
 * lu_parent pointers outside the triple that a prune has to rewire.
 */
struct prune_candidate {
  centree_node leaf;  /* L */
  centree_node inner; /* L->lu_parent, the younger internal node */
  centree_node outer; /* inner->lu_parent, the older internal node */
  centree_node up;    /* D, outer->lu_parent; NULL at the history root */
  centree_node sib;   /* A, inner's other history child */
  centree_node star;  /* *, outer's other history child */
  int side;           /* s, the side of leaf under inner */
  size_t cold_bound;  /* upper bound on the valid entries of outer + inner */
};

/* Selection only; neither mutates anything. */
bool prune_select(centree_node leaf, struct prune_candidate *out);
bool prune_scan_for_candidate(struct prune_candidate *out);
size_t prune_count_candidates(void);

/*
 * The stale-slot estimate: over every node in the routing tree, slots ever
 * reserved (last_item) against entries still believed valid (nb_items, which
 * every invalidation and every abandoned slot decrements). stale = reserved -
 * valid. It undercounts (a missed invalidation, a stale-but-unmarked entry
 * carried into N, both count as valid) and never overcounts, so acting on it
 * never prunes too eagerly. A hint, not a snapshot: the counters are read
 * without locks.
 */
struct prune_stale {
  size_t nodes;
  size_t reserved;
  size_t valid;
  size_t stale;
};
void prune_stale_measure(struct prune_stale *out);
double prune_stale_ratio(const struct prune_stale *m);

/*
 * The replacement node N under construction. Nothing points at it until the
 * history link and routing splice, so an unfinished build can simply be
 * discarded.
 */
struct prune_build {
  struct slab *slab;  /* N */
  centree_node node;  /* N's node, unpublished */
  char *buffer;       /* page-aligned image of the pages N will use */
  size_t capacity;    /* slots the buffer holds */
  size_t count;       /* slots filled so far */
  size_t dirty_lo;    /* pages staged but not written yet, [lo, hi) */
  size_t dirty_hi;
  centree_node pivot_from; /* Q: the routing position N will take */
};

/*
 * begin -> add_source (newest first) -> finish. Every step returns 0 or a
 * negative errno; begin() zeroes *out on failure, and any other failure
 * leaves the build discardable. `pivot_from` is the node whose routing
 * position N will take: its pivot, level and file key. `override` lets a
 * higher-precedence source replace a record already staged, which is what the
 * leaf needs since it joins after the two internal slabs.
 */
int prune_build_begin(const struct prune_candidate *c, centree_node pivot_from,
                      struct prune_build *out);
int prune_build_add_source(struct prune_build *b, centree_node source,
                           int override);
/* Writes the staged pages out; keeps the build open. */
int prune_build_flush(struct prune_build *b);
int prune_build_finish(struct prune_build *b);
/* Both source slabs, oldest last. Equivalent to begin + add + add + finish. */
int prune_build_cold(const struct prune_candidate *c, struct prune_build *out);
/* Unlinks N's file and frees the build. Only valid while unpublished. */
void prune_build_discard(struct prune_build *b);

/*
 * Freeze the leaf, copy it into N, and splice N into the history chain.
 * Returns 0 with *b holding the finished N, or a negative errno with nothing
 * changed (-EAGAIN: the routing shape was not what selection saw;
 * -EBUSY/-ENOSPC from the freeze). After the freeze succeeds there is no abort
 * path: only the routing splice can release the writers waiting on the frozen
 * leaf.
 *
 * The caller must hold tnt_maintenance_lock() from here until the routing
 * splice has published, so that P and Q cannot be rewired underneath.
 */
int prune_freeze_and_link(const struct prune_candidate *c,
                          struct prune_build *b);

/*
 * Put N in Q's routing position, drop P and the leaf out of the tree, publish,
 * then retire the triple and wake the writers parked on the frozen leaf.
 * Must follow a successful prune_freeze_and_link() under the same
 * tnt_maintenance_lock() hold. It does not fail: a routing shape that
 * contradicts what was recorded means an invariant is broken, and there is no
 * safe rollback once N is in the history chain.
 */
void prune_splice_routing(const struct prune_candidate *c,
                          struct prune_build *b);

/*
 * Hand back what the triple was holding: local indexes now, files as soon as
 * the last reader or writer lets go. The nodes, the slab descriptors and
 * hot_bits stay allocated on purpose -- they are held raw across async I/O
 * and by the reinsertion queue.
 */
void prune_retire(const struct prune_candidate *c);

enum tnt_prune_status {
  TNT_PRUNE_DONE = 0,
  TNT_PRUNE_NOOP = 1, /* nothing was prunable */
};

/*
 * One prune, start to finish, under tnt_maintenance_lock(): select, build,
 * freeze, link, splice, retire. Returns a status above or a negative errno.
 * -EAGAIN/-EBUSY/-ENOSPC mean the candidate was dropped with nothing changed;
 * they are normal races, not failures. Safe while clients are running, but
 * only one caller at a time is intended.
 */
int tnt_prune_once(void);

/*
 * Slots the pruner found holding a record other than the one the local index
 * named. Not caused by pruning; see the comment in prune_build_add_source().
 */
uint64_t prune_bad_slot_count(void);

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
