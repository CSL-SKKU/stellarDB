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

typedef struct centree_node_t {
  void* key;
  tree_entry_t value;
  struct rcu_ptr left;
  struct rcu_ptr right;
  struct rcu_ptr parent;
  struct centree_node_t* lu_parent;
  _Atomic int child_flag;
  unsigned char removed;
  // unsigned char gc;
} * centree_node;

typedef struct centree_t {
  centree_node root;
  struct rcu_ctx topology_rcu;
  _Atomic uint64_t depth;
  _Atomic uint64_t node_count;
} * centree;

/*
 * Routing-pointer access. read_* requires one surrounding
 * centree_read_in()/centree_read_out() pair. current_* is for callers that
 * already prevent topology publication (currently centree_root_lock).
 */
static inline void centree_read_in(centree tree) {
  rcu_read_in(&tree->topology_rcu);
}

static inline void centree_read_out(centree tree) {
  rcu_read_out(&tree->topology_rcu);
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

static inline void centree_node_init_links(centree_node node,
                                           centree_node left,
                                           centree_node right,
                                           centree_node parent) {
  rcu_ptr_init(&node->left, left);
  rcu_ptr_init(&node->right, right);
  rcu_ptr_init(&node->parent, parent);
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

uint64_t centree_get_depth(centree t);
centree centree_create();
tree_entry_t* centree_lookup(centree t, void* key, compare_func compare);
tree_entry_t* centree_traverse_useq(centree t, int seq);
centree_node centree_insert(centree t, void* key, tree_entry_t* value,
                            compare_func compare);
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

/* The caller must hold centree_root_lock. */
bool centree_validate_locked(centree tree);

/* Returns zero on success/no-op, otherwise an errno value. */
int centree_balance(centree tree);

#endif
