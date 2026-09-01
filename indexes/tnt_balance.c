#include "tnt_centree.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct node_snapshot {
  centree_node left;
  centree_node right;
  centree_node parent;
  centree_node lu_parent;
  unsigned char removed;
  bool was_leaf;
};

static bool validate_subtree(centree_node node, size_t *count) {
  bool has_left = node->left != NULL;
  bool has_right = node->right != NULL;

  if (has_left != has_right)
    return false;
  if (has_left && node->left == node->right)
    return false;
  if (has_left &&
      (node->left->parent != node || node->right->parent != node))
    return false;
  if (*count == SIZE_MAX)
    return false;

  (*count)++;
  if (!has_left)
    return true;

  return validate_subtree(node->left, count) &&
         validate_subtree(node->right, count);
}

bool centree_validate_locked(centree tree) {
  size_t count = 0;

  if (tree == NULL || tree->root == NULL || tree->root->parent != NULL)
    return false;
  if (!validate_subtree(tree->root, &count))
    return false;

  return count % 2 == 1;
}

static size_t count_nodes(centree_node node) {
  if (node == NULL)
    return 0;
  return 1 + count_nodes(node->left) + count_nodes(node->right);
}

static bool collect_nodes(centree_node node, centree_node *nodes,
                          size_t capacity, size_t *index) {
  if (node == NULL)
    return true;
  if (!collect_nodes(node->left, nodes, capacity, index))
    return false;
  if (*index == capacity)
    return false;
  nodes[(*index)++] = node;
  return collect_nodes(node->right, nodes, capacity, index);
}

static void mark_fixed(centree_node node) {
  if (node == NULL)
    return;
  if (node->left == NULL && node->right == NULL) {
    node->removed = 1;
  } else {
    node->removed = 0;
    mark_fixed(node->left);
    mark_fixed(node->right);
  }
}

static int build_tree_from_array(centree_node *nodes, size_t l, size_t r,
                                 centree_node parent,
                                 centree_node *result) {
  size_t count = r - l;

  if (count == 0 || count % 2 == 0)
    return EINVAL;
  if (count == 1) {
    centree_node node = nodes[l];

    if (node->removed != 1)
      return EINVAL;
    node->left = NULL;
    node->right = NULL;
    node->parent = parent;
    *result = node;
    return 0;
  }

  size_t best_k = SIZE_MAX;
  size_t best_diff = SIZE_MAX;
  for (size_t k = l + 1; k < r - 1; k += 2) {
    size_t left_size = k - l;
    size_t right_size = r - k - 1;
    size_t diff = left_size > right_size ? left_size - right_size
                                          : right_size - left_size;

    if (diff < best_diff) {
      best_diff = diff;
      best_k = k;
    }
  }
  if (best_k == SIZE_MAX)
    return EINVAL;

  centree_node root = nodes[best_k];
  centree_node left;
  centree_node right;
  int error;

  if (root->removed != 0)
    return EINVAL;
  error = build_tree_from_array(nodes, l, best_k, root, &left);
  if (error != 0)
    return error;
  error = build_tree_from_array(nodes, best_k + 1, r, root, &right);
  if (error != 0)
    return error;

  root->parent = parent;
  root->left = left;
  root->right = right;
  *result = root;
  return 0;
}

static void restore_tree(centree tree, centree_node old_root,
                         centree_node *nodes,
                         const struct node_snapshot *snapshots,
                         size_t count) {
  for (size_t i = 0; i < count; i++) {
    nodes[i]->left = snapshots[i].left;
    nodes[i]->right = snapshots[i].right;
    nodes[i]->parent = snapshots[i].parent;
    nodes[i]->lu_parent = snapshots[i].lu_parent;
    nodes[i]->removed = snapshots[i].removed;
  }
  tree->root = old_root;
}

static unsigned int refresh_routing_metadata(centree_node node,
                                             centree_node parent,
                                             unsigned int level) {
  unsigned int actual_depth = level;

  node->parent = parent;
  node->value.level = level;
  node->removed = 0;

  if (node->left != NULL) {
    unsigned int left_depth =
        refresh_routing_metadata(node->left, node, level + 1);

    if (left_depth > actual_depth)
      actual_depth = left_depth;
  }
  if (node->right != NULL) {
    unsigned int right_depth =
        refresh_routing_metadata(node->right, node, level + 1);

    if (right_depth > actual_depth)
      actual_depth = right_depth;
  }

  return actual_depth;
}

int centree_balance(centree tree) {
  centree_node old_root;
  centree_node new_root;
  centree_node *nodes = NULL;
  centree_node *after = NULL;
  struct node_snapshot *snapshots = NULL;
  size_t total_nodes;
  size_t index = 0;
  int error = 0;

  if (tree == NULL)
    return EINVAL;
  if (tree->root == NULL)
    return 0;
  if (!centree_validate_locked(tree))
    return EINVAL;

  old_root = tree->root;
  total_nodes = count_nodes(old_root);
  if (total_nodes > SIZE_MAX / sizeof(*nodes) ||
      total_nodes > SIZE_MAX / sizeof(*snapshots))
    return ENOMEM;

  nodes = malloc(total_nodes * sizeof(*nodes));
  after = malloc(total_nodes * sizeof(*after));
  snapshots = malloc(total_nodes * sizeof(*snapshots));
  if (nodes == NULL || after == NULL || snapshots == NULL) {
    error = ENOMEM;
    goto out;
  }
  if (!collect_nodes(old_root, nodes, total_nodes, &index) ||
      index != total_nodes) {
    error = EINVAL;
    goto out;
  }

  for (size_t i = 0; i < total_nodes; i++) {
    snapshots[i].left = nodes[i]->left;
    snapshots[i].right = nodes[i]->right;
    snapshots[i].parent = nodes[i]->parent;
    snapshots[i].lu_parent = nodes[i]->lu_parent;
    snapshots[i].removed = nodes[i]->removed;
    snapshots[i].was_leaf = nodes[i]->left == NULL;
  }

  mark_fixed(old_root);
  for (size_t i = 0; i < total_nodes; i++) {
    unsigned char expected = i % 2 == 0 ? 1 : 0;

    if (nodes[i]->removed != expected) {
      error = EINVAL;
      goto restore;
    }
  }

  error = build_tree_from_array(nodes, 0, total_nodes, NULL, &new_root);
  if (error != 0)
    goto restore;
  new_root->parent = NULL;
  tree->root = new_root;

  index = 0;
  if (!centree_validate_locked(tree) ||
      !collect_nodes(tree->root, after, total_nodes, &index) ||
      index != total_nodes) {
    error = EFAULT;
    goto restore;
  }
  for (size_t i = 0; i < total_nodes; i++) {
    bool is_leaf = nodes[i]->left == NULL && nodes[i]->right == NULL;

    if (after[i] != nodes[i] || is_leaf != snapshots[i].was_leaf ||
        nodes[i]->lu_parent != snapshots[i].lu_parent) {
      error = EFAULT;
      goto restore;
    }
  }

  unsigned int actual_depth =
      refresh_routing_metadata(tree->root, NULL, 1);
  atomic_store(&tree->depth, actual_depth);
  goto out;

restore:
  restore_tree(tree, old_root, nodes, snapshots, total_nodes);
out:
  free(snapshots);
  free(after);
  free(nodes);
  return error;
}
