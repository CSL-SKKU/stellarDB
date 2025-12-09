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
#include <unistd.h>
#include <stdint.h>
#include <stdatomic.h>

#include "memory-item.h"

typedef struct centree_node_t {
  void* key;
  tree_entry_t value;
  struct centree_node_t* left;
  struct centree_node_t* right;
  struct centree_node_t* parent;
  struct centree_node_t* lu_parent;
  _Atomic int child_flag;
  unsigned char removed;
  // unsigned char gc;
} * centree_node;

typedef struct centree_t {
  centree_node root;
  _Atomic uint64_t depth;
} * centree;

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

void centree_balance(centree t);

#endif
