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

static node new_node(void *key, tree_entry_t *value, node left, node right);
static node lookup_node(centree t, void *key, compare_func compare);

int tnt_pointer_cmp(void *left, void *right) {
  if (left > right) {
    return 1;
  } else if (left < right) {
    return -1;
  } else if (left == right) {
    return 0;
  }
  return 0;  // Pleases GCC
}
centree centree_create() {
  centree t = malloc(sizeof(struct centree_t));
  t->root = NULL;
  atomic_init(&t->depth, 0);
  atomic_init(&t->node_count, 0);
  bgqueue = malloc(sizeof(background_queue));
  init_queue(bgqueue);
  return t;
}

node new_node(void *key, tree_entry_t *value, node left, node right) {
  node result = malloc(sizeof(struct centree_node_t));
  result->removed = 0;
  result->key = key;
  result->value = *value;
  result->left = left;
  result->right = right;
  result->lu_parent = NULL;
  atomic_store(&result->child_flag, 0);
  if (left != NULL) left->parent = result;
  if (right != NULL) right->parent = result;
  result->parent = NULL;
  return result;
}

node lookup_node(centree t, void *key, compare_func compare) {
  node n = t->root;
  while (n != NULL) {
    int comp_result = compare(key, n->key);
    if (comp_result == 0) {
      return n;
    } else if (comp_result < 0) {
      n = n->left;
    } else {
      assert(comp_result > 0);
      n = n->right;
    }
  }
  return n;
}

uint64_t centree_get_depth(centree t) {
  return atomic_load_explicit(&t->depth, memory_order_acquire);
}

tree_entry_t *centree_lookup(centree t, void *key, compare_func compare) {
  node n = lookup_node(t, key, compare);
  return n == NULL ? NULL : &n->value;
}

node centree_insert(centree t, void *key, tree_entry_t *value,
                    compare_func compare) {
  node inserted_node = new_node(key, value, NULL, NULL);
  uint64_t level = 1;

  if (t->root == NULL) {
    t->root = inserted_node;
  } else {
    node n = t->root;
    while (1) {
      int comp_result = compare(key, n->key);

      level++;
      if (comp_result <= 0) {
        if (n->left == NULL) {
          n->left = inserted_node;
          break;
        } else {
          n = n->left;
        }
      } else {
        assert(comp_result > 0);
        if (n->right == NULL) {
          n->right = inserted_node;
          break;
        } else {
          n = n->right;
        }
      }
    }
    inserted_node->parent = n;
    inserted_node->lu_parent = n;
  }
  if (atomic_load_explicit(&t->depth, memory_order_acquire) < level)
    atomic_store_explicit(&t->depth, level, memory_order_release);
  value->level = level;
  inserted_node->value = *value;
  atomic_fetch_add_explicit(&t->node_count, 1, memory_order_release);
  return inserted_node;
}

node centree_insert_dual(centree t, void *key, 
                         void *lk, void *rk, 
                         tree_entry_t *lv, tree_entry_t *rv, 
                         compare_func compare) {
  uint64_t level = 0;
  node n = t->root;

  if (n == NULL) {
    return NULL;
  } else {
    while (1) {
      int comp_result = compare(key, n->key);

      level++;
      if (comp_result < 0) {
        if (n->left == NULL) {
          return NULL;
        } else {
          n = n->left;
        }
      } else if (comp_result > 0) {
        if (n->right == NULL) {
          return NULL;
        } else {
          n = n->right;
        }
      } else {
        n->left = new_node(lk, lv, NULL, NULL);
        n->right = new_node(rk, rv, NULL, NULL);
        lv->level = level;
        rv->level = level;
        n->left->parent = n;
        n->right->parent = n;
        n->left->lu_parent = n;
        n->right->lu_parent = n;
        atomic_fetch_add_explicit(&t->node_count, 2, memory_order_release);
        break;
      }
    }
  }

  return n;
}
    
node traverse_node_useq(centree t, int key) {
  node n = NULL;
  if (key == 0) {  // init
    enqueue_centnode(bgqueue, t->root);
  }
  if (!is_empty(bgqueue)) {
    n = dequeue_centnode(bgqueue);
    if (n->left) enqueue_centnode(bgqueue, n->left);
    if (n->right) enqueue_centnode(bgqueue, n->right);
  }
  return n;
}

tree_entry_t *centree_traverse_useq(centree t, int seq) {
  node n = traverse_node_useq(t, seq);
  return n == NULL ? NULL : &n->value;
}

// Function to print binary tree in 2D
// It does reverse inorder traversal
void print2DUtil(node n, int space) {
  // Base case
  if (n == NULL) return;

  // Increase distance between levels
  space += 10;

  // Process right child first
  print2DUtil(n->right, space);

  // Print current node after space
  // count
  printf("\n");
  for (int i = 1; i < space; i++) printf(" ");
  if (n->value.slab->min != -1) {
    if (cfg.with_reins) {
      printf("%lu,%lu:%lu//%lu,%lu,%lu\n", n->value.seq, n->value.level,
             n->value.slab->nb_items, 
             atomic_load_explicit(&n->value.slab->cur_ep, memory_order_relaxed), 
             atomic_load_explicit(&n->value.slab->prev_epcnt, memory_order_relaxed), 
             atomic_load_explicit(&n->value.slab->epcnt, memory_order_relaxed)
             );
    } else {
      printf("%lu,%lu:%lu\n", n->value.seq, n->value.level,
             n->value.slab->nb_items
             );
    }
  }
  else
    printf("%lu,%lu:0\n", n->value.seq, n->value.level);

  // Process left child
  print2DUtil(n->left, space);
}

void centree_print_nodes(node n, compare_func show) {
  if (!n) return;

  printf("l\n");
  centree_print_nodes(n->left, show);
  show(n->key, &n->value);
  printf("r\n");
  centree_print_nodes(n->right, show);
}

void centree_print(centree t) {
  node n = t->root;
  print2DUtil(n, 0);
  // centree_print_nodes(n, show);
}
