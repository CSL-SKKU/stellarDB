#include "headers.h"

#include <errno.h>

int load = 0;
int print = 0;
int rc_thr = 1;

static void add_node(struct slab *slab, uint64_t key) {
  memset(slab, 0, sizeof(*slab));
  slab->key = key;
  slab->seq = key;
  tnt_subtree_add(slab, tnt_subtree_create(), NULL, key);
}

int main(void) {
  static const uint64_t additional_keys[] = {
      16384, 49152, 8192, 24576, 4096, 12288,
      2048, 6144, 1024, 3072, 512, 1536,
  };
  struct slab slabs[15];

  /* The threshold is read from cfg; unset it is 0 and every tree is "deep". */
  init_default_config(&cfg);
  assert(!cfg.with_rebal && cfg.rebalance_threshold == REBALANCE_THRESHOLD);

  assert(get_distributor_utilization() == 0);
  assert(get_io_worker_utilization() == 0);
  assert(tnt_rebalancing() == -EINVAL);
  assert(tnt_get_node_count() == 0);
  assert(!tnt_rebalancing_needed());

  centree_init();
  assert(tnt_rebalancing() == TNT_REBALANCE_NOOP);

  add_node(&slabs[0], 65536);
  assert(tnt_get_node_count() == 1);
  assert(!tnt_rebalancing_needed());
  centree_node root = slabs[0].centree_node;
  rcu_ptr_init(&root->parent, root);
  assert(tnt_rebalancing() == -EINVAL);
  rcu_ptr_init(&root->parent, NULL);

  root->value.level = 42;
  root->removed = 1;
  assert(tnt_rebalancing() == TNT_REBALANCE_NOOP);
  assert(root->value.level == 1);
  assert(root->removed == 0);
  assert(tnt_get_depth() == 1);

  add_node(&slabs[1], 32768);
  add_node(&slabs[2], 98304);
  assert(tnt_get_node_count() == 3);
  assert(!tnt_rebalancing_needed());
  assert(tnt_rebalancing() == TNT_REBALANCE_SUCCESS);
  assert(tnt_get_depth() == 2);

  assert(tnt_rebalancing() == TNT_REBALANCE_SUCCESS);
  assert(tnt_get_depth() == 2);

  for (size_t i = 0; i < sizeof(additional_keys) / sizeof(additional_keys[0]);
       i++)
    add_node(&slabs[i + 3], additional_keys[i]);
  assert(tnt_get_node_count() == 15);
  assert(tnt_get_depth() == 8);

  /* A larger threshold is less sensitive. Equality must not trigger. */
  cfg.rebalance_threshold = 5.0;
  assert(!tnt_rebalancing_needed());
  atomic_store(&tnt_centree()->depth, 20); /* 5 * ceil(log2(16)) */
  assert(!tnt_rebalancing_needed());
  atomic_store(&tnt_centree()->depth, 21);
  assert(tnt_rebalancing_needed());
  atomic_store(&tnt_centree()->depth, 8);
  cfg.rebalance_threshold = 2.0;
  assert(!tnt_rebalancing_needed());
  cfg.rebalance_threshold = 1.5;
  assert(tnt_rebalancing_needed());

  assert(tnt_rebalancing() == TNT_REBALANCE_SUCCESS);
  assert(tnt_get_node_count() == 15);
  assert(tnt_get_depth() == 4);
  assert(!tnt_rebalancing_needed());
  cfg.rebalance_threshold = 1.0;
  assert(!tnt_rebalancing_needed());
  cfg.rebalance_threshold = 0.0;
  assert(tnt_rebalancing_needed());
  cfg.rebalance_threshold = REBALANCE_THRESHOLD;

  puts("rebalance runtime API tests passed");
  return 0;
}
