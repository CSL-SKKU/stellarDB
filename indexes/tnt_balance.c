#include "tnt_centree.h"

#include <assert.h>
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

struct centree_balance_plan {
  centree tree;
  centree_node new_root;
  centree_node *nodes;
  struct node_snapshot *snapshots;
  struct rcu_writer writer;
  size_t total_nodes;
  unsigned int actual_depth;
};

static void free_plan(struct centree_balance_plan *plan) {
  if (plan == NULL)
    return;
  free(plan->snapshots);
  free(plan->nodes);
  free(plan);
}

int centree_balance_prepare(centree tree,
                            struct centree_balance_plan **out_plan) {
  struct centree_balance_plan *plan = NULL;
  centree_node *after = NULL;
  size_t index = 0;
  bool writer_active = false;
  int error = 0;

  if (out_plan == NULL)
    return EINVAL;
  *out_plan = NULL;
  if (tree == NULL)
    return EINVAL;
  if (tree->root == NULL)
    return 0;
  if (!centree_validate_locked(tree))
    return EINVAL;

  plan = calloc(1, sizeof(*plan));
  if (plan == NULL)
    return ENOMEM;
  plan->tree = tree;
  plan->total_nodes = count_nodes(tree, tree->root);
  if (plan->total_nodes > SIZE_MAX / sizeof(*plan->nodes) ||
      plan->total_nodes > SIZE_MAX / sizeof(*plan->snapshots)) {
    error = ENOMEM;
    goto out;
  }

  plan->nodes = malloc(plan->total_nodes * sizeof(*plan->nodes));
  after = malloc(plan->total_nodes * sizeof(*after));
  plan->snapshots =
      malloc(plan->total_nodes * sizeof(*plan->snapshots));
  if (plan->nodes == NULL || after == NULL || plan->snapshots == NULL) {
    error = ENOMEM;
    goto out;
  }
  if (!collect_nodes(tree, tree->root, plan->nodes, plan->total_nodes,
                     &index) ||
      index != plan->total_nodes) {
    error = EINVAL;
    goto out;
  }

  for (size_t i = 0; i < plan->total_nodes; i++) {
    plan->snapshots[i].lu_parent = plan->nodes[i]->lu_parent;
    plan->snapshots[i].level = plan->nodes[i]->value.level;
    plan->snapshots[i].removed = plan->nodes[i]->removed;
    plan->snapshots[i].was_leaf =
        centree_current_left(tree, plan->nodes[i]) == NULL;
  }

  mark_fixed(tree, tree->root);
  for (size_t i = 0; i < plan->total_nodes; i++) {
    unsigned char expected = i % 2 == 0 ? 1 : 0;

    if (plan->nodes[i]->removed != expected) {
      error = EINVAL;
      goto restore;
    }
  }

  plan->writer = rcu_writer_in(&tree->topology_rcu);
  writer_active = true;
  error = build_tree_from_array(&plan->writer, plan->nodes, 0,
                                plan->total_nodes, &plan->new_root);
  if (error != 0)
    goto restore;

  plan->actual_depth =
      refresh_routing_metadata(&plan->writer, plan->new_root, NULL, 1);

  size_t count = 0;
  index = 0;
  if (writer_parent(&plan->writer, plan->new_root) != NULL ||
      !validate_writer_subtree(&plan->writer, plan->new_root, &count) ||
      count % 2 != 1 ||
      !collect_writer_nodes(&plan->writer, plan->new_root, after,
                            plan->total_nodes, &index) ||
      index != plan->total_nodes) {
    error = EFAULT;
    goto restore;
  }
  for (size_t i = 0; i < plan->total_nodes; i++) {
    bool is_leaf = writer_left(&plan->writer, plan->nodes[i]) == NULL &&
                   writer_right(&plan->writer, plan->nodes[i]) == NULL;

    if (after[i] != plan->nodes[i] ||
        is_leaf != plan->snapshots[i].was_leaf ||
        plan->nodes[i]->lu_parent != plan->snapshots[i].lu_parent) {
      error = EFAULT;
      goto restore;
    }
  }

  free(after);
  *out_plan = plan;
  return 0;

restore:
  if (writer_active)
    rcu_writer_abort(&tree->topology_rcu, &plan->writer);
  restore_metadata(plan->nodes, plan->snapshots, plan->total_nodes);
out:
  free(after);
  free_plan(plan);
  return error;
}

void centree_balance_publish(struct centree_balance_plan *plan) {
  assert(plan != NULL);

  plan->tree->root = plan->new_root;
  atomic_store(&plan->tree->depth, plan->actual_depth);
  rcu_writer_publish_deferred(&plan->tree->topology_rcu, &plan->writer,
                              NULL, NULL);
}

void centree_balance_complete(struct centree_balance_plan *plan) {
  assert(plan != NULL);

  rcu_writer_finish_deferred(&plan->tree->topology_rcu, &plan->writer);
  free_plan(plan);
}

void centree_balance_commit(struct centree_balance_plan *plan) {
  centree_balance_publish(plan);
  centree_balance_complete(plan);
}

void centree_balance_abort(struct centree_balance_plan *plan) {
  if (plan == NULL)
    return;

  rcu_writer_abort(&plan->tree->topology_rcu, &plan->writer);
  restore_metadata(plan->nodes, plan->snapshots, plan->total_nodes);
  free_plan(plan);
}

int centree_balance(centree tree) {
  struct centree_balance_plan *plan;
  int error = centree_balance_prepare(tree, &plan);

  if (error != 0)
    return error;
  if (plan != NULL)
    centree_balance_commit(plan);
  return 0;
}
