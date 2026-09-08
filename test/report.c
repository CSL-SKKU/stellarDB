#include "headers.h"
#include <errno.h>

int print, load;

/* Test the entry partition independently of async scheduling. Every mutation
 * below represents an indexed record, including repeat invalidation, slot
 * replacement, in-place DELETE/resurrection, and prune's dense slot move. */
static void counts(subtree_t *t, uint64_t total, uint64_t stale, uint64_t tombstones) {
  uint64_t a = 0, b = 0, c = 0;
  subtree_report_counts(t, &a, &b, &c);
  assert(a == total && b == stale && c == tombstones);
}
static void put(subtree_t *t, uint64_t key, uint64_t slot, int tombstone) {
  index_entry_t e = {.slab_idx = slot};
  subtree_insert(t, (unsigned char *)&key, sizeof(key), &e);
  subtree_report_record(t, key, slot, tombstone);
}

static struct slab hop_slabs[5];
static centree_node retry_node;
static int retry_state, retry_migration;

/* Retire an ancestor on the first pass, then clear it on the restarted pass;
 * migration instead retries the same node without crossing another edge. */
static void retry_hook(centree_node n) {
  if (n != retry_node || retry_state == 2) return;
  int value = retry_state++ == 0;
  if (retry_migration) atomic_store(&n->value.slab->superseded, value);
  else atomic_store(&n->removed, value);
}

static void lookup(uint64_t key, struct slab *want, unsigned upward_len, int client) {
  struct {
    struct item_metadata meta;
    uint64_t key;
  } item = {.key = key};
  item_init(&item.meta, sizeof(key), 0);
  struct slab_callback cb = {0};
  index_entry_t *e = client ? tnt_index_lookup_client(&cb, &item)
                           : tnt_index_lookup(&cb, &item);
  assert((e ? e->slab : NULL) == want);
  if (want) assert(cb.upward_len == upward_len);
  tnt_index_lookup_unref(e);
}

static void add_hop_slab(unsigned i, uint64_t pivot, uint64_t key) {
  struct slab *s = &hop_slabs[i];
  s->key = pivot;
  s->seq = i + 1;
  s->min = s->max = key;
  INIT_LOCK(&s->tree_lock, NULL);
  tnt_subtree_add(s, tnt_subtree_create(), NULL, pivot);
  put(s->subtree, key, 0, 0);
}

static void *thread_reads(void *unused) {
  (void)unused;
  for (int i = 0; i < 3; i++) lookup(20, &hop_slabs[1], 2, 1);
  return NULL;
}

static void check_hops(void) {
  add_hop_slab(0, 100, 10);
  lookup(10, &hop_slabs[0], 1, 1); /* root is leaf: 0 up, 0 down */
  add_hop_slab(1, 50, 20);
  add_hop_slab(2, 150, 160);
  add_hop_slab(3, 25, 30);
  add_hop_slab(4, 75, 80);
  lookup(10, &hop_slabs[0], 3, 1); /* 2 up, 2 down */
  lookup(20, &hop_slabs[1], 2, 1); /* 1 up, 2 down */
  lookup(30, &hop_slabs[3], 1, 1); /* 0 up, 2 down */
  lookup(40, NULL, 0, 1);          /* miss: 2 up, 2 down; NULL is no edge */
  retry_node = hop_slabs[1].centree_node;
  tnt_set_index_lookup_step_test_hook(retry_hook);
  lookup(10, &hop_slabs[0], 3, 1); /* restarted: 3 up, 4 down */
  assert(retry_state == 2);
  retry_state = 0;
  retry_migration = 1;
  lookup(10, &hop_slabs[0], 3, 1); /* same-node retry: 2 up, 2 down */
  assert(retry_state == 2);
  tnt_set_index_lookup_step_test_hook(NULL);
  lookup(10, &hop_slabs[0], 3, 0); /* maintenance lookup is excluded */
  pthread_t thread;
  assert(pthread_create(&thread, NULL, thread_reads, NULL) == 0);
  assert(pthread_join(thread, NULL) == 0); /* 3 reads: 3 up, 6 down */
  /* Ten reads, thirteen upward edges, twenty downward edges. */
  for (unsigned i = 0; i < 5; i++) assert(hop_slabs[i].read_ref == 0);
}

int main(void) {
  report_mask = (UINT64_C(1) << REPORT_ENTRIES_STALE) |
                (UINT64_C(1) << REPORT_ENTRIES_TOMBSTONES);
  subtree_t *t = subtree_create();
  uint64_t a = 11, b = 22;
  put(t, a, 0, 0);
  put(t, b, 1024, 1); /* grow the optional bitmap */
  counts(t, 2, 0, 1);
  subtree_report_record(t, a, 0, 1); /* in-place DELETE */
  counts(t, 2, 0, 2);
  subtree_report_record(t, a, 0, 0); /* resurrection */
  counts(t, 2, 0, 1);
  subtree_report_record(t, a, 7, 1); /* losing reservation must not change it */
  counts(t, 2, 0, 1);
  assert(subtree_set_invalid(t, (unsigned char *)&b, sizeof(b)) == 1);
  assert(subtree_set_invalid(t, (unsigned char *)&b, sizeof(b)) == 0);
  counts(t, 2, 1, 0);
  subtree_delete(t, (unsigned char *)&b, sizeof(b));
  put(t, b, 1, 1);
  counts(t, 2, 0, 1);
  subtree_delete(t, (unsigned char *)&a, sizeof(a));
  subtree_delete(t, (unsigned char *)&b, sizeof(b));
  put(t, b, 0, 1); /* last staged tombstone moved into a vacated slot */
  counts(t, 1, 0, 1);
  subtree_free(t);
  assert(subtree_marked_total() == 0);

  report_mask = 0;
  t = subtree_create();
  put(t, a, 99999, 1);
  assert(t->report_tombstones == NULL && t->live_tombstones == 0);
  subtree_free(t);
  puts("PASS entry classes, repeated invalidation, replacement, dense slot moves, reporting off");

  char config[] = "/tmp/stellar-report-config-XXXXXX";
  char output[] = "/tmp/stellar-report-output-XXXXXX";
  int fd = mkstemp(config), out = mkstemp(output);
  assert(fd >= 0 && out >= 0);
  FILE *f = fdopen(fd, "w");
  fputs("all=false\nrebalance_attempts=true\nupward_hops_avg=true\ndownward_hops_avg=true\n", f);
  fclose(f);
  close(out); unlink(output);
  assert(report_init(output, config, 0) == 0);
  report_read_hops(100, 200); /* excluded setup */
  assert(tnt_rebalancing() == -EINVAL); /* excluded setup */
  report_begin();
  assert(tnt_rebalancing() == -EINVAL); /* abandoned attempt still counts */
  centree_init();
  assert(tnt_rebalancing() == TNT_REBALANCE_NOOP); /* no-op still counts */
  check_hops();
  report_finish();
  lookup(10, &hop_slabs[0], 3, 1); /* excluded post-run work */
  assert(tnt_rebalancing() == TNT_REBALANCE_SUCCESS);
  report_close();
  f = fopen(output, "r");
  char line[256];
  assert(f && fgets(line, sizeof(line), f));
  assert(!strcmp(line, "time_s,upward_hops_avg,downward_hops_avg,rebalance_attempts\n"));
  assert(fgets(line, sizeof(line), f));
  assert(!strcmp(strchr(line, ','), ",1.300000000,2.000000000,2\n"));
  assert(fgets(line, sizeof(line), f) == NULL);
  fclose(f); unlink(config); unlink(output);
  puts("PASS attempt semantics and measured-run event boundaries");
  puts("PASS traversal edges, hits, misses, retry hops, maintenance exclusion, thread-weighted averages");
}
