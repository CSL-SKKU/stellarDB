/* Copyright (c) 2011 the authors listed at the following URL, and/or
the authors of referenced articles or incorporated external code:
http://en.literateprograms.org/Red-black_tree_(C)?action=history&offset=20090121005050

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Retrieved from: http://en.literateprograms.org/Red-black_tree_(C)?oldid=16016
... and modified for even more speed and awesomeness...
*/

#ifndef _CENTREE_H_
#define _CENTREE_H_ 1
#include <stdbool.h>
#include <unistd.h>
#include <stdint.h>
#include <stdatomic.h>

#include "memory-item.h"
#include "../rcu.h"

/*
 * Lifetime contract until pruning/reclamation is implemented:
 *
 * Once published, center-tree nodes and their slab descriptors remain
 * allocated for the lifetime of the database process. RCU protects routing
 * generation consistency, not object reclamation. Raw node/tree_entry/slab
 * pointers may therefore outlive a read section, but only because this
 * no-reclamation rule is in force.
 *
 * Future pruning must either retain the RCU read section through the final
 * object access, or acquire a stable object reference and retire the removed
 * node/slab only after a grace period.
 */
typedef struct centree_node_t {
  _Atomic(uint64_t) key;
  tree_entry_t value;
  struct rcu_ptr left;
  struct rcu_ptr right;
  struct rcu_ptr parent;
  /*
   * History links, used by the upward (authoritative version) lookup.
   * lu_parent is assigned once when a split creates the node and is mutated
   * only by pruning, while walkers read it without holding any lock: hence the
   * atomic. lu_child[] are its back-pointers (CENTREE_LU_LEFT/RIGHT), written
   * only by the splitter before publication and by the single pruner, so they
   * stay plain.
   */
  _Atomic(struct centree_node_t *) lu_parent;
  struct centree_node_t* lu_child[2];
  _Atomic int child_flag;
  /* Advisory maintenance metadata; never used to choose a routing edge. */
  _Atomic(unsigned char) removed;
  // unsigned char gc;
} * centree_node;

typedef struct centree_t {
  struct rcu_ptr root;
  struct rcu_ctx topology_rcu;
  _Atomic uint64_t depth;
  _Atomic uint64_t node_count;
} * centree;

/*
 * Routing-pointer access. read_* requires one surrounding
 * centree_read_in()/centree_read_out() pair. current_* is for callers that
 * already prevent topology publication (currently centree_root_lock).
 * Keeping a returned node after read_out() relies on the lifetime contract
 * above; it does not pin the routing generation.
 */
static inline void centree_read_in(centree tree) {
  rcu_read_in(&tree->topology_rcu);
}

static inline void centree_read_out(centree tree) {
  rcu_read_out(&tree->topology_rcu);
}

static inline centree_node centree_read_root(centree tree) {
  return (centree_node)rcu_read_ptr(&tree->topology_rcu, &tree->root);
}

static inline centree_node centree_read_left(centree tree,
                                             centree_node node) {
  return (centree_node)rcu_read_ptr(&tree->topology_rcu, &node->left);
}

static inline centree_node centree_read_right(centree tree,
                                              centree_node node) {
  return (centree_node)rcu_read_ptr(&tree->topology_rcu, &node->right);
}

static inline centree_node centree_read_parent(centree tree,
                                               centree_node node) {
  return (centree_node)rcu_read_ptr(&tree->topology_rcu, &node->parent);
}

static inline centree_node centree_current_root(centree tree) {
  return (centree_node)rcu_current_ptr(&tree->topology_rcu, &tree->root);
}

static inline centree_node centree_current_left(centree tree,
                                                centree_node node) {
  return (centree_node)rcu_current_ptr(&tree->topology_rcu, &node->left);
}

static inline centree_node centree_current_right(centree tree,
                                                 centree_node node) {
  return (centree_node)rcu_current_ptr(&tree->topology_rcu, &node->right);
}

static inline centree_node centree_current_parent(centree tree,
                                                  centree_node node) {
  return (centree_node)rcu_current_ptr(&tree->topology_rcu, &node->parent);
}

/*
 * Routing pointers in the generation the single topology writer is building.
 * Reads see that writer's own uncommitted changes, which is what lets a
 * multi-pointer restructure be staged consistently.
 */
static inline centree_node centree_writer_left(struct rcu_writer *writer,
                                               centree_node node) {
  return (centree_node)rcu_writer_ptr(writer, &node->left);
}

static inline centree_node centree_writer_right(struct rcu_writer *writer,
                                                centree_node node) {
  return (centree_node)rcu_writer_ptr(writer, &node->right);
}

static inline centree_node centree_writer_parent(struct rcu_writer *writer,
                                                 centree_node node) {
  return (centree_node)rcu_writer_ptr(writer, &node->parent);
}

static inline void centree_writer_set_left(struct rcu_writer *writer,
                                           centree_node node,
                                           centree_node value) {
  rcu_writer_set_ptr(writer, &node->left, value);
}

static inline void centree_writer_set_right(struct rcu_writer *writer,
                                            centree_node node,
                                            centree_node value) {
  rcu_writer_set_ptr(writer, &node->right, value);
}

static inline void centree_writer_set_parent(struct rcu_writer *writer,
                                             centree_node node,
                                             centree_node value) {
  rcu_writer_set_ptr(writer, &node->parent, value);
}

static inline centree_node centree_writer_root(struct rcu_writer *writer,
                                               centree tree) {
  return (centree_node)rcu_writer_ptr(writer, &tree->root);
}

static inline void centree_writer_set_root(struct rcu_writer *writer,
                                           centree tree, centree_node value) {
  rcu_writer_set_ptr(writer, &tree->root, value);
}

/* Construction only: the node must not be reachable from a published root. */
static inline void centree_node_init_links(centree_node node,
                                           centree_node left,
                                           centree_node right,
                                           centree_node parent) {
  rcu_ptr_init(&node->left, left);
  rcu_ptr_init(&node->right, right);
  rcu_ptr_init(&node->parent, parent);
}

static inline uint64_t centree_pivot_load(centree_node node) {
  return atomic_load_explicit(&node->key, memory_order_acquire);
}

static inline void centree_pivot_store(centree_node node, uint64_t pivot) {
  atomic_store_explicit(&node->key, pivot, memory_order_release);
}

#define CENTREE_LU_LEFT 0
#define CENTREE_LU_RIGHT 1

/*
 * History-link access. lu_parent is the only link that pruning rewires while
 * readers are walking it without a lock, so loads acquire and stores are
 * seq_cst.
 */
static inline centree_node centree_lu_parent(centree_node node) {
  return atomic_load_explicit(&node->lu_parent, memory_order_acquire);
}

static inline void centree_lu_parent_store(centree_node node,
                                           centree_node parent) {
  atomic_store_explicit(&node->lu_parent, parent, memory_order_seq_cst);
}

typedef struct bgq_node_t {
  union {
    struct centree_node_t* data;
    char* item;
  };
  struct bgq_node_t* next;
} bgq_node;

typedef struct background_queue_t {
  struct bgq_node_t* front;
  struct bgq_node_t* rear;
  int count;  // 큐 안의 노드 개수
} background_queue;

typedef int (*compare_func)(void* left, void* right);
int tnt_pointer_cmp(void* left, void* right);

/* compare(requested_key, pivot): left for <, right for >=. */
static inline bool centree_route_left(int comparison) {
  return comparison < 0;
}

uint64_t centree_get_depth(centree t);
centree centree_create();
/* The returned entry remains allocated under the no-reclamation contract. */
tree_entry_t* centree_lookup(centree t, void* key, compare_func compare);
tree_entry_t* centree_traverse_useq(centree t, int seq);
/*
 * Stage one insertion in writer's inactive generation. The caller owns the
 * topology writer and prevents concurrent current-generation mutation.
 * Publication remains the caller's responsibility.
 */
centree_node centree_insert(centree t, struct rcu_writer *writer, void* key,
                            tree_entry_t* value, compare_func compare);
/*
 * Allocate an unlinked node. Pruning builds its replacement node before any
 * pointer to it exists; the node obeys the same no-reclamation rule as the
 * ones centree_insert creates.
 */
centree_node centree_node_new(void* key, tree_entry_t* value);
/*
 * node_count is advisory (it feeds the rebalance trigger). Pruning replaces
 * three nodes with one, so it has to give two back.
 */
void centree_node_count_sub(centree t, uint64_t count);
// void centree_delete(centree t, void* key, compare_func compare);

void centree_print(centree t);

void init_queue(background_queue* queue);
int is_empty(background_queue* queue);
void enqueue_centnode(background_queue* queue, centree_node n);
centree_node dequeue_centnode(background_queue* queue);

struct centree_scan_tmp {
  struct centree_node_t* entries;
  size_t nb_entries;
};

/* The caller must prevent concurrent topology publication. */
bool centree_validate_locked(centree tree);

struct centree_balance_plan;

/*
 * Prepare rewires only the unpublished RCU slot. The caller must prevent
 * splits and other topology writers until it commits or aborts the plan.
 * Publish must run while new root-lock readers are excluded. Complete does
 * the retired-slot cleanup and may run after releasing that lock.
 */
int centree_balance_prepare(centree tree,
                            struct centree_balance_plan **out_plan);
void centree_balance_publish(struct centree_balance_plan *plan);
void centree_balance_complete(struct centree_balance_plan *plan);
void centree_balance_commit(struct centree_balance_plan *plan);
void centree_balance_abort(struct centree_balance_plan *plan);

/* Returns zero on success/no-op, otherwise an errno value. */
int centree_balance(centree tree);

#endif
