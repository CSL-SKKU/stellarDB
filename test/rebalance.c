#include "indexes/tnt_centree.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void init_node(struct centree_node_t *node, uintptr_t key) {
  memset(node, 0, sizeof(*node));
  node->key = (void *)key;
}

static void link_children(struct centree_node_t *parent,
                          struct centree_node_t *left,
                          struct centree_node_t *right) {
  parent->left = left;
  parent->right = right;
  left->parent = parent;
  right->parent = parent;
}

static void collect_inorder(centree_node node, centree_node *nodes,
                            size_t *index) {
  if (node == NULL) return;
  collect_inorder(node->left, nodes, index);
  nodes[(*index)++] = node;
  collect_inorder(node->right, nodes, index);
}

static void test_valid_rebalance(void) {
  static const unsigned int expected_levels[7] = {3, 2, 3, 1, 3, 2, 3};
  struct centree_node_t nodes[7];
  struct centree_t tree = {0};
  centree_node before[7];
  centree_node after[7];
  centree_node lu_parents[7];
  void *keys[7];
  uint64_t value_keys[7];
  uint64_t seqs[7];
  struct slab *slabs[7];
  size_t index = 0;

  for (size_t i = 0; i < 7; i++) {
    init_node(&nodes[i], i + 1);
    nodes[i].lu_parent = i == 0 ? &nodes[0] : &nodes[i - 1];
    nodes[i].value.key = 100 + i;
    nodes[i].value.seq = 200 + i;
    nodes[i].value.level = 90 + i;
    nodes[i].value.slab = (struct slab *)(uintptr_t)(300 + i);
    lu_parents[i] = nodes[i].lu_parent;
    keys[i] = nodes[i].key;
    value_keys[i] = nodes[i].value.key;
    seqs[i] = nodes[i].value.seq;
    slabs[i] = nodes[i].value.slab;
  }

  link_children(&nodes[1], &nodes[0], &nodes[3]);
  link_children(&nodes[3], &nodes[2], &nodes[5]);
  link_children(&nodes[5], &nodes[4], &nodes[6]);
  tree.root = &nodes[1];
  atomic_store(&tree.depth, 99);

  assert(centree_validate_locked(&tree));
  collect_inorder(tree.root, before, &index);
  assert(index == 7);

  assert(centree_balance(&tree) == 0);
  assert(tree.root == &nodes[3]);
  assert(centree_validate_locked(&tree));
  assert(atomic_load(&tree.depth) == 3);

  index = 0;
  collect_inorder(tree.root, after, &index);
  assert(index == 7);
  for (size_t i = 0; i < 7; i++) {
    assert(after[i] == before[i]);
    assert(nodes[i].lu_parent == lu_parents[i]);
    assert(nodes[i].key == keys[i]);
    assert(nodes[i].value.key == value_keys[i]);
    assert(nodes[i].value.seq == seqs[i]);
    assert(nodes[i].value.slab == slabs[i]);
    assert(nodes[i].value.level == expected_levels[i]);
    assert(nodes[i].removed == 0);
    assert((nodes[i].left == NULL && nodes[i].right == NULL) ==
           (i % 2 == 0));
  }

  assert(centree_balance(&tree) == 0);
  assert(atomic_load(&tree.depth) == 3);
  for (size_t i = 0; i < 7; i++) {
    assert(nodes[i].value.level == expected_levels[i]);
    assert(nodes[i].removed == 0);
  }
}

static void test_single_node_metadata(void) {
  struct centree_node_t root;
  struct centree_t tree = {0};

  init_node(&root, 1);
  root.value.level = 42;
  root.removed = 1;
  tree.root = &root;
  atomic_store(&tree.depth, 42);

  assert(centree_balance(&tree) == 0);
  assert(tree.root == &root);
  assert(root.parent == NULL);
  assert(root.value.level == 1);
  assert(root.removed == 0);
  assert(atomic_load(&tree.depth) == 1);
}

static void test_invalid_trees_return_errors(void) {
  struct centree_t empty = {0};
  struct centree_node_t root;
  struct centree_node_t left;
  struct centree_node_t right;
  struct centree_t tree = {0};

  assert(!centree_validate_locked(NULL));
  assert(centree_balance(NULL) == EINVAL);
  assert(!centree_validate_locked(&empty));
  assert(centree_balance(&empty) == 0);

  init_node(&root, 2);
  init_node(&left, 1);
  root.left = &left;
  left.parent = &root;
  tree.root = &root;
  assert(!centree_validate_locked(&tree));
  assert(centree_balance(&tree) == EINVAL);
  assert(tree.root == &root);
  assert(root.left == &left && root.right == NULL);

  init_node(&root, 2);
  init_node(&left, 1);
  init_node(&right, 3);
  root.left = &left;
  root.right = &right;
  right.parent = &root;
  tree.root = &root;
  assert(!centree_validate_locked(&tree));
  assert(centree_balance(&tree) == EINVAL);

  init_node(&root, 2);
  init_node(&left, 1);
  root.left = &left;
  root.right = &left;
  left.parent = &root;
  tree.root = &root;
  assert(!centree_validate_locked(&tree));
  assert(centree_balance(&tree) == EINVAL);
}

int main(void) {
  test_valid_rebalance();
  test_single_node_metadata();
  test_invalid_trees_return_errors();
  puts("rebalance structural tests passed");
  return 0;
}
