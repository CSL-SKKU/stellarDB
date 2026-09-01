#include "tnt_centree.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct node_snapshot {
  centree_node lu_parent;
  uint64_t level;
  unsigned char removed;
  bool was_leaf;
};

static bool validate_subtree(centree tree, centree_node node,
                             size_t *count) {
  centree_node left = centree_current_left(tree, node);
  centree_node right = centree_current_right(tree, node);
  bool has_left = left != NULL;
  bool has_right = right != NULL;

  if (has_left != has_right)
    return false;
  if (has_left && left == right)
    return false;
  if (has_left &&
      (centree_current_parent(tree, left) != node ||
       centree_current_parent(tree, right) != node))
    return false;
  if (*count == SIZE_MAX)
    return false;

  (*count)++;
  if (!has_left)
    return true;

  return validate_subtree(tree, left, count) &&
         validate_subtree(tree, right, count);
}

bool centree_validate_locked(centree tree) {
  size_t count = 0;

  if (tree == NULL || tree->root == NULL ||
      centree_current_parent(tree, tree->root) != NULL)
    return false;
  if (!validate_subtree(tree, tree->root, &count))
    return false;

  return count % 2 == 1;
}

static size_t count_nodes(centree tree, centree_node node) {
  if (node == NULL)
    return 0;
  return 1 + count_nodes(tree, centree_current_left(tree, node)) +
         count_nodes(tree, centree_current_right(tree, node));
}

static bool collect_nodes(centree tree, centree_node node,
                          centree_node *nodes, size_t capacity,
                          size_t *index) {
  if (node == NULL)
    return true;
  if (!collect_nodes(tree, centree_current_left(tree, node), nodes, capacity,
                     index))
    return false;
  if (*index == capacity)
    return false;
  nodes[(*index)++] = node;
  return collect_nodes(tree, centree_current_right(tree, node), nodes,
                       capacity, index);
}

static void mark_fixed(centree tree, centree_node node) {
  centree_node left;
  centree_node right;

  if (node == NULL)
    return;
  left = centree_current_left(tree, node);
  right = centree_current_right(tree, node);
  if (left == NULL && right == NULL) {
    node->removed = 1;
  } else {
    node->removed = 0;
    mark_fixed(tree, left);
    mark_fixed(tree, right);
  }
}

static centree_node writer_left(struct rcu_writer *writer,
                                centree_node node) {
  return rcu_writer_ptr(writer, &node->left);
}

static centree_node writer_right(struct rcu_writer *writer,
                                 centree_node node) {
  return rcu_writer_ptr(writer, &node->right);
}

static centree_node writer_parent(struct rcu_writer *writer,
                                  centree_node node) {
  return rcu_writer_ptr(writer, &node->parent);
}

static int build_tree_from_array(struct rcu_writer *writer,
                                 centree_node *nodes, size_t l, size_t r,
                                 centree_node *result) {
  size_t count = r - l;

  if (count == 0 || count % 2 == 0)
    return EINVAL;
  if (count == 1) {
    centree_node node = nodes[l];

    if (node->removed != 1)
      return EINVAL;
    rcu_writer_set_ptr(writer, &node->left, NULL);
    rcu_writer_set_ptr(writer, &node->right, NULL);
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
  error = build_tree_from_array(writer, nodes, l, best_k, &left);
  if (error != 0)
    return error;
  error = build_tree_from_array(writer, nodes, best_k + 1, r, &right);
  if (error != 0)
    return error;

  rcu_writer_set_ptr(writer, &root->left, left);
  rcu_writer_set_ptr(writer, &root->right, right);
  *result = root;
  return 0;
}

static unsigned int refresh_routing_metadata(struct rcu_writer *writer,
                                             centree_node node,
                                             centree_node parent,
                                             unsigned int level) {
  centree_node left = writer_left(writer, node);
  centree_node right = writer_right(writer, node);
  unsigned int actual_depth = level;

  rcu_writer_set_ptr(writer, &node->parent, parent);
  node->value.level = level;
  node->removed = 0;

  if (left != NULL) {
    unsigned int left_depth =
        refresh_routing_metadata(writer, left, node, level + 1);

    if (left_depth > actual_depth)
      actual_depth = left_depth;
  }
  if (right != NULL) {
    unsigned int right_depth =
        refresh_routing_metadata(writer, right, node, level + 1);

    if (right_depth > actual_depth)
      actual_depth = right_depth;
  }

  return actual_depth;
}

static bool validate_writer_subtree(struct rcu_writer *writer,
                                    centree_node node, size_t *count) {
  centree_node left = writer_left(writer, node);
  centree_node right = writer_right(writer, node);
  bool has_left = left != NULL;
  bool has_right = right != NULL;

  if (has_left != has_right || (has_left && left == right))
    return false;
  if (has_left &&
      (writer_parent(writer, left) != node ||
       writer_parent(writer, right) != node))
    return false;
  if (*count == SIZE_MAX)
    return false;

  (*count)++;
  if (!has_left)
    return true;
  return validate_writer_subtree(writer, left, count) &&
         validate_writer_subtree(writer, right, count);
}

static bool collect_writer_nodes(struct rcu_writer *writer,
                                 centree_node node, centree_node *nodes,
                                 size_t capacity, size_t *index) {
  if (node == NULL)
    return true;
  if (!collect_writer_nodes(writer, writer_left(writer, node), nodes,
                            capacity, index))
    return false;
  if (*index == capacity)
    return false;
  nodes[(*index)++] = node;
  return collect_writer_nodes(writer, writer_right(writer, node), nodes,
                              capacity, index);
}

static void restore_metadata(centree_node *nodes,
                             const struct node_snapshot *snapshots,
                             size_t count) {
  for (size_t i = 0; i < count; i++) {
    nodes[i]->value.level = snapshots[i].level;
    nodes[i]->removed = snapshots[i].removed;
  }
}

int centree_balance(centree tree) {
  centree_node new_root;
  centree_node *nodes = NULL;
  centree_node *after = NULL;
  struct node_snapshot *snapshots = NULL;
  struct rcu_writer writer = {0};
  size_t total_nodes;
  size_t index = 0;
  unsigned int actual_depth = 0;
  bool writer_active = false;
  int error = 0;

  if (tree == NULL)
    return EINVAL;
  if (tree->root == NULL)
    return 0;
  if (!centree_validate_locked(tree))
    return EINVAL;

  total_nodes = count_nodes(tree, tree->root);
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
  if (!collect_nodes(tree, tree->root, nodes, total_nodes, &index) ||
      index != total_nodes) {
    error = EINVAL;
    goto out;
  }

  for (size_t i = 0; i < total_nodes; i++) {
    snapshots[i].lu_parent = nodes[i]->lu_parent;
    snapshots[i].level = nodes[i]->value.level;
    snapshots[i].removed = nodes[i]->removed;
    snapshots[i].was_leaf = centree_current_left(tree, nodes[i]) == NULL;
  }

  mark_fixed(tree, tree->root);
  for (size_t i = 0; i < total_nodes; i++) {
    unsigned char expected = i % 2 == 0 ? 1 : 0;

    if (nodes[i]->removed != expected) {
      error = EINVAL;
      goto restore;
    }
  }

  writer = rcu_writer_in(&tree->topology_rcu);
  writer_active = true;
  error = build_tree_from_array(&writer, nodes, 0, total_nodes, &new_root);
  if (error != 0)
    goto restore;

  actual_depth = refresh_routing_metadata(&writer, new_root, NULL, 1);

  size_t count = 0;
  index = 0;
  if (writer_parent(&writer, new_root) != NULL ||
      !validate_writer_subtree(&writer, new_root, &count) || count % 2 != 1 ||
      !collect_writer_nodes(&writer, new_root, after, total_nodes, &index) ||
      index != total_nodes) {
    error = EFAULT;
    goto restore;
  }
  for (size_t i = 0; i < total_nodes; i++) {
    bool is_leaf = writer_left(&writer, nodes[i]) == NULL &&
                   writer_right(&writer, nodes[i]) == NULL;

    if (after[i] != nodes[i] || is_leaf != snapshots[i].was_leaf ||
        nodes[i]->lu_parent != snapshots[i].lu_parent) {
      error = EFAULT;
      goto restore;
    }
  }

  tree->root = new_root;
  rcu_writer_publish(&tree->topology_rcu, &writer, NULL, NULL);
  atomic_store(&tree->depth, actual_depth);
  goto out;

restore:
  if (writer_active)
    rcu_writer_abort(&tree->topology_rcu, &writer);
  restore_metadata(nodes, snapshots, total_nodes);
out:
  free(snapshots);
  free(after);
  free(nodes);
  return error;
}
