/* Copyright (c)2011 the authors listed at the following URL, and/or
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
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO
EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

Retrieved from: http://en.literateprograms.org/Red-black_tree_(C)?oldid=16016
*/

#include "tnt_centree.h"
#include "../headers.h"
#include "btree.h"
#include <assert.h>

#include <stdlib.h>
#include <stdio.h>

typedef centree_node node;

static background_queue *bgqueue;

void init_queue(background_queue *queue) {
  queue->front = queue->rear = NULL;
  queue->count = 0;  // 큐 안의 노드 개수를 0으로 설정
}

int is_empty(background_queue *queue) {
  return queue->count == 0 ;  // 큐안의 노드 개수가 0이면 빈 상태
}

void enqueue_centnode(background_queue *queue, node n) {
  bgq_node *new = (bgq_node *)malloc(sizeof(bgq_node));  // newNode 생성
  new->data = n;
  new->next = NULL;

  if (is_empty(queue))  // 큐가 비어있을 때
  {
    queue->front = new;
  } else  // 비어있지 않을 때
  {
    queue->rear->next = new;  //맨 뒤의 다음을 newNode로 설정
  }
  queue->rear = new;  //맨 뒤를 newNode로 설정
  queue->count++;     //큐안의 노드 개수를 1 증가
}

node dequeue_centnode(background_queue *queue) {
  node data;
  bgq_node *ptr;
  if (is_empty(queue))  //큐가 비었을 때
  {
    printf("Error : Queue is empty!\n");
    return 0;
  }
  ptr = queue->front;        //맨 앞의 노드 ptr 설정
  data = ptr->data;          // return 할 데이터
  queue->front = ptr->next;  //맨 앞은 ptr의 다음 노드로 설정
  free(ptr);                 // ptr 해제
  queue->count--;            //큐의 노드 개수를 1 감소

  return data;
}

static node new_node(void *key, tree_entry_t *value);
static node lookup_node(centree t, void *key, compare_func compare);

int tnt_pointer_cmp(void *left, void *right) {
  uint64_t left_key = (uint64_t)(uintptr_t)left;
  uint64_t right_key = (uint64_t)(uintptr_t)right;

  if (left_key > right_key) {
    return 1;
  } else if (left_key < right_key) {
    return -1;
  } else if (left_key == right_key) {
    return 0;
  }
  return 0;  // Pleases GCC
}
centree centree_create() {
  centree t = malloc(sizeof(struct centree_t));
  rcu_init(&t->topology_rcu);
  rcu_ptr_init(&t->root, NULL);
  atomic_init(&t->depth, 0);
  atomic_init(&t->node_count, 0);
  bgqueue = malloc(sizeof(background_queue));
  init_queue(bgqueue);
  return t;
}

node new_node(void *key, tree_entry_t *value) {
  /* Ownership passes to the center tree; live node reclamation is disabled. */
  node result = malloc(sizeof(struct centree_node_t));
  atomic_init(&result->removed, 0);
  atomic_init(&result->key, (uint64_t)(uintptr_t)key);
  result->value = *value;
  centree_node_init_links(result, NULL, NULL, NULL);
  atomic_init(&result->lu_parent, NULL);
  result->lu_child[CENTREE_LU_LEFT] = NULL;
  result->lu_child[CENTREE_LU_RIGHT] = NULL;
  atomic_store(&result->child_flag, 0);
  return result;
}

centree_node centree_node_new(void *key, tree_entry_t *value) {
  return new_node(key, value);
}

void centree_node_count_sub(centree t, uint64_t count) {
  uint64_t old = atomic_fetch_sub_explicit(&t->node_count, count,
                                           memory_order_release);

  assert(old >= count);
  (void)old;
}

node lookup_node(centree t, void *key, compare_func compare) {
  node n = centree_read_root(t);
  while (n != NULL) {
    int comp_result =
        compare(key, (void *)(uintptr_t)centree_pivot_load(n));
    if (comp_result == 0) {
      return n;
    } else if (centree_route_left(comp_result)) {
      n = centree_read_left(t, n);
    } else {
      assert(comp_result > 0);
      n = centree_read_right(t, n);
    }
  }
  return n;
}

uint64_t centree_get_depth(centree t) {
  return atomic_load_explicit(&t->depth, memory_order_acquire);
}

tree_entry_t *centree_lookup(centree t, void *key, compare_func compare) {
  centree_read_in(t);
  node n = lookup_node(t, key, compare);
  tree_entry_t *result = n == NULL ? NULL : &n->value;
  centree_read_out(t);
  return result;
}

node centree_insert(centree t, struct rcu_writer *writer, void *key,
                    tree_entry_t *value, compare_func compare) {
  assert(writer != NULL && writer->ctx == &t->topology_rcu);

  node inserted_node = new_node(key, value);
  node root = centree_current_root(t);
  uint64_t level = 1;

  if (root == NULL) {
    rcu_writer_set_ptr(writer, &t->root, inserted_node);
  } else {
    node n = root;
    while (1) {
      int comp_result =
          compare(key, (void *)(uintptr_t)centree_pivot_load(n));

      level++;
      if (centree_route_left(comp_result)) {
        node left = centree_current_left(t, n);

        if (left == NULL) {
          value->level = level;
          inserted_node->value = *value;
          centree_lu_parent_store(inserted_node, n);
          /* A routing leaf is a history leaf, so the slot must be free. */
          assert(n->lu_child[CENTREE_LU_LEFT] == NULL);
          n->lu_child[CENTREE_LU_LEFT] = inserted_node;
          rcu_writer_set_ptr(writer, &inserted_node->parent, n);
          rcu_writer_set_ptr(writer, &n->left, inserted_node);
          break;
        } else {
          n = left;
        }
      } else {
        assert(comp_result >= 0);
        node right = centree_current_right(t, n);

        if (right == NULL) {
          value->level = level;
          inserted_node->value = *value;
          centree_lu_parent_store(inserted_node, n);
          /* A routing leaf is a history leaf, so the slot must be free. */
          assert(n->lu_child[CENTREE_LU_RIGHT] == NULL);
          n->lu_child[CENTREE_LU_RIGHT] = inserted_node;
          rcu_writer_set_ptr(writer, &inserted_node->parent, n);
          rcu_writer_set_ptr(writer, &n->right, inserted_node);
          break;
        } else {
          n = right;
        }
      }
    }
  }
  if (atomic_load_explicit(&t->depth, memory_order_acquire) < level)
    atomic_store_explicit(&t->depth, level, memory_order_release);
  value->level = level;
  inserted_node->value = *value;
  atomic_fetch_add_explicit(&t->node_count, 1, memory_order_release);
  return inserted_node;
}
    
node traverse_node_useq(centree t, int key) {
  node n = NULL;
  if (key == 0) {  // init
    enqueue_centnode(bgqueue, centree_current_root(t));
  }
  if (!is_empty(bgqueue)) {
    n = dequeue_centnode(bgqueue);
    node left = centree_current_left(t, n);
    node right = centree_current_right(t, n);

    if (left) enqueue_centnode(bgqueue, left);
    if (right) enqueue_centnode(bgqueue, right);
  }
  return n;
}

tree_entry_t *centree_traverse_useq(centree t, int seq) {
  node n = traverse_node_useq(t, seq);
  return n == NULL ? NULL : &n->value;
}

// Function to print binary tree in 2D
// It does reverse inorder traversal
void print2DUtil(centree t, node n, int space) {
  // Base case
  if (n == NULL) return;

  // Increase distance between levels
  space += 10;

  // Process right child first
  print2DUtil(t, centree_current_right(t, n), space);

  // Print current node after space
  // count
  printf("\n");
  for (int i = 1; i < space; i++) printf(" ");
  if (n->value.slab->min != -1) {
    printf("%lu,%lu:%lu\n", n->value.seq, n->value.level,
           n->value.slab->nb_items);
  }
  else
    printf("%lu,%lu:0\n", n->value.seq, n->value.level);

  // Process left child
  print2DUtil(t, centree_current_left(t, n), space);
}

void centree_print_nodes(centree t, node n, compare_func show) {
  if (!n) return;

  printf("l\n");
  centree_print_nodes(t, centree_current_left(t, n), show);
  show((void *)(uintptr_t)centree_pivot_load(n), &n->value);
  printf("r\n");
  centree_print_nodes(t, centree_current_right(t, n), show);
}

void centree_print(centree t) {
  node n = centree_current_root(t);
  print2DUtil(t, n, 0);
  // centree_print_nodes(t, n, show);
}
