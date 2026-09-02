#include "headers.h"

#include <stdbool.h>

#define TEST_ITEM_SIZE 64
#define TEST_KEY_SIZE sizeof(uint64_t)

int load = 0;
int rc_thr = 1;

void read_item_async_cb(struct slab_callback *callback);
int add_existing_item(struct slab *s, size_t idx, void *item,
                      struct slab_callback *cb);

static int callback_count;
static void *callback_item;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      fprintf(stderr, "DELETE test failed at %s:%d: %s\n", __FILE__,       \
              __LINE__, #condition);                                         \
      exit(EXIT_FAILURE);                                                    \
    }                                                                        \
  } while (0)

static void capture_read(struct slab_callback *cb, void *item) {
  (void)cb;
  callback_count++;
  callback_item = item;
}

static struct slab *new_test_slab(uint64_t seq, uint64_t key,
                                  size_t max_items) {
  struct slab *s = calloc(1, sizeof(*s));
  CHECK(s != NULL);

  s->seq = seq;
  s->key = key;
  s->min = UINT64_MAX;
  s->max = 0;
  s->item_size = TEST_ITEM_SIZE;
  s->nb_max_items = max_items;
  s->size_on_disk = max_items * TEST_ITEM_SIZE;
  s->fd = -1;
  s->subtree = tnt_subtree_create();
  CHECK(s->subtree != NULL);
  subtree_set_slab(s->subtree, s);

  atomic_init(&s->full, 0);
  atomic_init(&s->last_item, 0);
  atomic_init(&s->queued, 0);
  atomic_init(&s->upward_maxlen, 0);
  atomic_init(&s->cur_ep, 0);
  atomic_init(&s->epcnt, 0);
  atomic_init(&s->prev_epcnt, 0);
  INIT_LOCK(&s->tree_lock, NULL);
  return s;
}

static unsigned char *new_test_item(uint64_t key, bool tombstone) {
  unsigned char *item = calloc(1, TEST_ITEM_SIZE);
  CHECK(item != NULL);

  struct item_metadata *meta = (struct item_metadata *)item;
  item_init(meta, TEST_KEY_SIZE,
            TEST_ITEM_SIZE - sizeof(*meta) - TEST_KEY_SIZE);
  *(uint64_t *)(item + sizeof(*meta)) = key;
  memset(item + sizeof(*meta) + TEST_KEY_SIZE, 0x5a, meta->value_size);
  if (tombstone)
    item_encode_tombstone(meta);
  return item;
}

static bool index_entry_is_invalid(const index_entry_t *entry) {
  return ((uint32_t)entry->slab_idx & (1u << 31)) != 0;
}

static void publish_direct(struct slab *s, size_t idx, void *item) {
  struct slab_callback cb = {
    .item = item,
    .slab = s,
    .slab_idx = idx,
  };
  struct item_metadata *meta = item;
  uint64_t key = *(uint64_t *)((char *)item + sizeof(*meta));

  tnt_index_add(&cb, item);
  if (key < s->min) s->min = key;
  if (key > s->max) s->max = key;
  s->nb_items++;
}

static void expect_read_result(struct slab *s, size_t idx, void *record,
                               bool expect_miss) {
  unsigned char *page = malloc(PAGE_SIZE);
  CHECK(page != NULL);
  memset(page, 0xa5, PAGE_SIZE);

  size_t offset = (idx % (PAGE_SIZE / s->item_size)) * s->item_size;
  struct item_metadata *meta = record;
  memcpy(page + offset, record, item_stored_size(meta));

  struct lru lru = {.page = page};
  struct slab_callback cb = {
    .cb = capture_read,
    .item = record,
    .slab = s,
    .slab_idx = idx,
    .lru_entry = &lru,
  };

  callback_count = 0;
  callback_item = (void *)(uintptr_t)1;
  s->read_ref = 1;
  read_item_async_cb(&cb);

  CHECK(callback_count == 1);
  if (expect_miss)
    CHECK(callback_item == NULL);
  else
    CHECK(callback_item == page + offset);
  CHECK(s->read_ref == 0);
  free(page);
}

static void test_tombstone_layout_and_reads(void) {
  unsigned char *normal = new_test_item(7, false);
  unsigned char *tombstone = new_test_item(7, true);
  struct item_metadata *tomb_meta = (struct item_metadata *)tombstone;

  CHECK(item_is_tombstone(tomb_meta));
  CHECK(item_key_size(tomb_meta) == TEST_KEY_SIZE);
  CHECK(tomb_meta->value_size == 0);
  CHECK(item_stored_size(tomb_meta) == sizeof(*tomb_meta) + TEST_KEY_SIZE);

  unsigned char slot[TEST_ITEM_SIZE];
  memset(slot, 0xa5, sizeof(slot));
  memcpy(slot, tombstone, item_stored_size(tomb_meta));
  CHECK(item_is_tombstone((struct item_metadata *)slot));
  CHECK(*(uint64_t *)(slot + sizeof(*tomb_meta)) == 7);
  CHECK(slot[item_stored_size(tomb_meta)] == 0xa5);

  struct slab *s = new_test_slab(100, 7, 8);
  s->min = s->max = 7;
  expect_read_result(s, 0, tombstone, true);
  expect_read_result(s, 0, normal, false);

  free(normal);
  free(tombstone);
}

static void test_recovery_indexes_tombstone(void) {
  struct slab *s = new_test_slab(101, 42, 8);
  unsigned char *tombstone = new_test_item(42, true);
  struct item_metadata *meta = (struct item_metadata *)tombstone;
  unsigned char slot[TEST_ITEM_SIZE];
  memset(slot, 0xa5, sizeof(slot));
  memcpy(slot, tombstone, item_stored_size(meta));

  struct slab_callback cb = {.slab = s};
  CHECK(add_existing_item(s, 0, slot, &cb) == 1);
  CHECK(cb.slab_idx == 0);
  CHECK(item_is_tombstone((struct item_metadata *)slot));

  index_entry_t *entry = tnt_index_lookup_utree(s->subtree, slot);
  CHECK(entry != NULL);
  CHECK(entry->slab == s);
  CHECK(GET_SIDX(entry->slab_idx) == 0);
  CHECK(!index_entry_is_invalid(entry));
  expect_read_result(s, 0, slot, true);

  unsigned char empty[TEST_ITEM_SIZE] = {0};
  CHECK(add_existing_item(s, 1, empty, &cb) == 0);
  free(tombstone);
}

static void test_routing_publication_and_invalidation(void) {
  centree_init();

  struct slab *root = new_test_slab(1, 50, 1);
  struct slab *left = new_test_slab(2, 49, 8);
  struct slab *right = new_test_slab(3, 51, 8);
  tnt_subtree_add(root, root->subtree, NULL, root->key);

  unsigned char *old_item = new_test_item(20, false);
  publish_direct(root, 0, old_item);
  atomic_store_explicit(&root->last_item, 1, memory_order_release);
  atomic_store_explicit(&root->full, 1, memory_order_release);

  tnt_subtree_add(left, left->subtree, NULL, left->key);
  tnt_subtree_add(right, right->subtree, NULL, right->key);
  wakeup_subtree_get(root->centree_node);

  unsigned char *moving_tombstone = new_test_item(20, true);
  uint64_t idx;
  index_entry_t *old_entry = NULL;
  struct tree_entry *tree =
      centree_lookup_and_reserve(moving_tombstone, &idx, &old_entry);
  CHECK(tree != NULL);
  CHECK(tree->slab == left);
  CHECK(idx == 0);
  CHECK(old_entry != NULL);
  CHECK(old_entry->slab == root);
  CHECK(GET_SIDX(old_entry->slab_idx) == 0);

  struct slab_callback move_cb = {
    .item = moving_tombstone,
    .slab = left,
    .slab_idx = idx,
    .fsst_slab = root,
    .fsst_idx = 0,
  };
  add_in_tree_for_upsert(&move_cb, moving_tombstone);

  index_entry_t *entry =
      tnt_index_lookup_utree(left->subtree, moving_tombstone);
  CHECK(entry != NULL);
  CHECK(entry->slab == left);
  CHECK(GET_SIDX(entry->slab_idx) == idx);
  CHECK(!index_entry_is_invalid(entry));

  entry = tnt_index_lookup_utree(root->subtree, moving_tombstone);
  CHECK(entry != NULL);
  CHECK(index_entry_is_invalid(entry));

  struct slab_callback lookup_cb = {.item = moving_tombstone};
  entry = tnt_index_lookup(&lookup_cb, moving_tombstone);
  CHECK(entry != NULL);
  CHECK(entry->slab == left);
  CHECK(GET_SIDX(entry->slab_idx) == idx);
  CHECK(left->read_ref == 1);
  tnt_index_lookup_unref(entry);
  CHECK(left->read_ref == 0);
  expect_read_result(left, idx, moving_tombstone, true);

  unsigned char *normal_in_leaf = new_test_item(10, false);
  old_entry = NULL;
  tree = centree_lookup_and_reserve(normal_in_leaf, &idx, &old_entry);
  CHECK(tree->slab == left);
  CHECK(old_entry == NULL);
  struct slab_callback add_cb = {
    .cb_cb = capture_read,
    .item = normal_in_leaf,
    .slab = left,
    .slab_idx = idx,
  };
  add_in_tree(&add_cb, normal_in_leaf);

  unsigned char *in_place_tombstone = new_test_item(10, true);
  old_entry = NULL;
  tree = centree_lookup_and_reserve(in_place_tombstone, &idx, &old_entry);
  CHECK(tree->slab == left);
  CHECK(idx == UINT64_MAX);
  CHECK(old_entry != NULL);
  CHECK(old_entry->slab == left);
  expect_read_result(left, GET_SIDX(old_entry->slab_idx),
                     in_place_tombstone, true);

  unsigned char *missing_tombstone = new_test_item(30, true);
  old_entry = NULL;
  tree = centree_lookup_and_reserve(missing_tombstone, &idx, &old_entry);
  CHECK(tree->slab == left);
  CHECK(idx != UINT64_MAX);
  CHECK(old_entry == NULL);
  struct slab_callback missing_cb = {
    .cb_cb = capture_read,
    .item = missing_tombstone,
    .slab = left,
    .slab_idx = idx,
  };
  add_in_tree(&missing_cb, missing_tombstone);
  entry = tnt_index_lookup_utree(left->subtree, missing_tombstone);
  CHECK(entry != NULL);
  CHECK(!index_entry_is_invalid(entry));
  expect_read_result(left, idx, missing_tombstone, true);

  CHECK(!item_is_empty((struct item_metadata *)moving_tombstone));
  CHECK(item_stored_size((struct item_metadata *)moving_tombstone) <
        left->item_size);

  unsigned char *resurrected = new_test_item(20, false);
  old_entry = NULL;
  tree = centree_lookup_and_reserve(resurrected, &idx, &old_entry);
  CHECK(tree->slab == left);
  CHECK(idx == UINT64_MAX);
  CHECK(old_entry != NULL);
  CHECK(old_entry->slab == left);
  expect_read_result(left, GET_SIDX(old_entry->slab_idx), resurrected, false);

  free(old_item);
  free(moving_tombstone);
  free(normal_in_leaf);
  free(in_place_tombstone);
  free(missing_tombstone);
  free(resurrected);
}

int main(void) {
  init_default_config(&cfg);
  cfg.kv_size = TEST_ITEM_SIZE;
  cfg.max_file_size = PAGE_SIZE;
  cfg.with_reins = 0;

  test_tombstone_layout_and_reads();
  test_recovery_indexes_tombstone();
  test_routing_publication_and_invalidation();

  puts("DELETE tests passed");
  return EXIT_SUCCESS;
}
