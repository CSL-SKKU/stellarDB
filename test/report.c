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
  fputs("all=false\nrebalance_attempts=true\n", f);
  fclose(f);
  close(out); unlink(output);
  assert(report_init(output, config, 0) == 0);
  assert(tnt_rebalancing() == -EINVAL); /* excluded setup */
  report_begin();
  assert(tnt_rebalancing() == -EINVAL); /* abandoned attempt still counts */
  centree_init();
  assert(tnt_rebalancing() == TNT_REBALANCE_NOOP); /* no-op still counts */
  report_finish();
  assert(tnt_rebalancing() == TNT_REBALANCE_NOOP); /* excluded post-run work */
  report_close();
  f = fopen(output, "r");
  char line[256];
  assert(f && fgets(line, sizeof(line), f));
  assert(!strcmp(line, "time_s,rebalance_attempts\n"));
  assert(fgets(line, sizeof(line), f));
  assert(!strcmp(strchr(line, ','), ",2\n"));
  assert(fgets(line, sizeof(line), f) == NULL);
  fclose(f); unlink(config); unlink(output);
  puts("PASS attempt semantics and measured-run event boundaries");
}
