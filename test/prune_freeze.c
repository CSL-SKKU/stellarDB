/*
 * slab_freeze() unit tests.
 *
 * Freeze is how the pruner makes a live leaf immutable. It must interlock
 * exactly with the writers:
 *
 *   - freeze and split are mutually exclusive. Both are decided by the same
 *     CAS on last_item, so for one slab either the pruner freezes it or a
 *     writer takes the final slot and becomes its splitter -- never both.
 *   - after a successful freeze no writer can reserve a slot.
 *   - a rejected candidate (-EBUSY / -ENOSPC) leaves the slab untouched.
 *   - freeze does not return while a writer still holds update_ref, so a
 *     write to a slot reserved before the freeze cannot land afterwards.
 *
 * No slab files and no workers: these only need last_item / full /
 * update_ref / nb_max_items.
 */
#include "headers.h"

#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>

int print = 0;
int load = 0;
int rc_thr = 1;

#define SLOTS 64
#define NB_WRITERS 4
#define RACE_ROUNDS 1200

static uint64_t failures;

static void check(bool ok, const char *fmt, ...) {
  if (ok) return;
  va_list ap;
  va_start(ap, fmt);
  printf("  FAIL ");
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
  failures++;
}

static void init_slab(struct slab *s, size_t reserved) {
  memset(s, 0, sizeof(*s));
  s->nb_max_items = SLOTS;
  s->item_size = 128;
  atomic_init(&s->full, 0);
  atomic_init(&s->last_item, reserved);
  atomic_init(&s->released, 0);
  INIT_LOCK(&s->tree_lock, NULL);
}

/* ------------------------------------------------------ sequential cases */

static void test_rejects(void) {
  struct slab s;

  /* Does not fit in the budget: nothing is frozen. */
  init_slab(&s, 5);
  check(slab_freeze(&s, 4) == -ENOSPC, "freeze over budget did not return -ENOSPC");
  check(atomic_load(&s.last_item) == 5, "over-budget freeze moved last_item");
  check(atomic_load(&s.full) == 0, "over-budget freeze published full");
  check(reserve_slot(&s) == 5, "writer cannot reserve after a rejected freeze");

  /* A writer already took the final slot: it owns the split. */
  init_slab(&s, SLOTS);
  check(slab_freeze(&s, SLOTS) == -EBUSY, "freeze of a filled slab did not return -EBUSY");
  check(atomic_load(&s.last_item) == SLOTS, "-EBUSY freeze moved last_item");

  /* Exact fit. */
  init_slab(&s, 5);
  check(slab_freeze(&s, 5) == 5, "exact-fit freeze failed");
  check(atomic_load(&s.last_item) == SLOTS, "freeze did not close last_item");
  check(atomic_load(&s.full) == 1, "freeze did not publish full");
  check(reserve_slot(&s) == (size_t)-1, "writer reserved a slot in a frozen slab");
  check(slab_freeze(&s, SLOTS) == -EBUSY, "second freeze did not return -EBUSY");
}

/* ----------------------------------------------------------- drain check */

static struct slab drain_slab;
static _Atomic int drain_done;
static long drain_ret;

static void *drain_worker(void *arg) {
  (void)arg;
  drain_ret = slab_freeze(&drain_slab, SLOTS);
  atomic_store(&drain_done, 1);
  return NULL;
}

static void test_drain(void) {
  pthread_t thread;

  init_slab(&drain_slab, 3);
  __sync_fetch_and_add(&drain_slab.update_ref, 1); /* a writer is mid-write */
  atomic_store(&drain_done, 0);
  pthread_create(&thread, NULL, drain_worker, NULL);

  usleep(100000);
  check(atomic_load(&drain_done) == 0,
        "freeze returned while a writer still held update_ref");
  /* full is published before the drain, so the writer's retry already fails. */
  check(atomic_load(&drain_slab.full) == 1, "freeze drains before publishing full");
  check(reserve_slot(&drain_slab) == (size_t)-1,
        "writer reserved a slot while the freeze was draining");

  __sync_fetch_and_sub(&drain_slab.update_ref, 1);
  for (int i = 0; i < 2000 && !atomic_load(&drain_done); i++) usleep(1000);
  check(atomic_load(&drain_done) == 1, "freeze did not finish after the drain");
  pthread_join(thread, NULL);
  check(drain_ret == 3, "freeze returned %ld, want 3", drain_ret);
}

/* ------------------------------------------------------------ freeze race */

static struct slab race_slab;
static _Atomic int race_go_writers;
static _Atomic int race_go_freezer;
static _Atomic int race_slots_taken;
static _Atomic long race_freeze_ret;

static void *race_writer(void *arg) {
  (void)arg;
  while (!atomic_load_explicit(&race_go_writers, memory_order_acquire)) ;
  if (reserve_slot(&race_slab) != (size_t)-1)
    atomic_fetch_add(&race_slots_taken, 1);
  return NULL;
}

static void *race_freezer(void *arg) {
  (void)arg;
  while (!atomic_load_explicit(&race_go_freezer, memory_order_acquire)) ;
  atomic_store(&race_freeze_ret, slab_freeze(&race_slab, SLOTS));
  return NULL;
}

/*
 * Four release patterns per round: both sides let go back to back in either
 * order (a real race), and either side given a clear head start (so both
 * outcomes are covered even if the race always resolves the same way).
 */
static void race_release(int mode) {
  switch (mode) {
    case 0:
      atomic_store_explicit(&race_go_writers, 1, memory_order_release);
      atomic_store_explicit(&race_go_freezer, 1, memory_order_release);
      break;
    case 1:
      atomic_store_explicit(&race_go_freezer, 1, memory_order_release);
      atomic_store_explicit(&race_go_writers, 1, memory_order_release);
      break;
    case 2:
      atomic_store_explicit(&race_go_freezer, 1, memory_order_release);
      usleep(50);
      atomic_store_explicit(&race_go_writers, 1, memory_order_release);
      break;
    default:
      atomic_store_explicit(&race_go_writers, 1, memory_order_release);
      usleep(50);
      atomic_store_explicit(&race_go_freezer, 1, memory_order_release);
      break;
  }
}

static void test_race(void) {
  size_t froze = 0, split = 0;

  for (int round = 0; round < RACE_ROUNDS; round++) {
    pthread_t writers[NB_WRITERS], freezer;

    /* One slot left: the next reservation is the one that triggers a split. */
    init_slab(&race_slab, SLOTS - 1);
    atomic_store(&race_go_writers, 0);
    atomic_store(&race_go_freezer, 0);
    atomic_store(&race_slots_taken, 0);
    atomic_store(&race_freeze_ret, 0);

    for (int i = 0; i < NB_WRITERS; i++)
      pthread_create(&writers[i], NULL, race_writer, NULL);
    pthread_create(&freezer, NULL, race_freezer, NULL);
    race_release(round % 4);

    for (int i = 0; i < NB_WRITERS; i++) pthread_join(writers[i], NULL);
    pthread_join(freezer, NULL);

    long r = atomic_load(&race_freeze_ret);
    int taken = atomic_load(&race_slots_taken);

    check(taken <= 1, "round %d: %d writers reserved the last slot", round, taken);
    if (r >= 0) {
      froze++;
      check(r == SLOTS - 1, "round %d: freeze returned %ld, want %d", round, r,
            SLOTS - 1);
      check(taken == 0, "round %d: a writer reserved a slot in a frozen slab",
            round);
    } else {
      split++;
      check(r == -EBUSY, "round %d: freeze returned %ld, want -EBUSY", round, r);
      check(taken == 1, "round %d: neither freeze nor a writer took the slot",
            round);
    }
    check(atomic_load(&race_slab.last_item) == SLOTS,
          "round %d: last_item %zu after the race", round,
          (size_t)atomic_load(&race_slab.last_item));
  }
  printf("  %-34s %zu froze, %zu split\n", "freeze vs. final slot", froze, split);
  check(froze > 0 && split > 0, "the race only ever went one way (%zu/%zu)",
        froze, split);
}

static struct slab wait_slab;
static _Atomic int wait_done;

static void *wait_worker(void *arg) {
  (void)arg;
  slab_drain_updates(&wait_slab);
  atomic_store(&wait_done, 1);
  return NULL;
}

static void test_drain_updates(void) {
  pthread_t thread;

  init_slab(&wait_slab, 3);
  __sync_fetch_and_add(&wait_slab.update_ref, 1);
  atomic_store(&wait_done, 0);
  pthread_create(&thread, NULL, wait_worker, NULL);
  usleep(100000);
  check(atomic_load(&wait_done) == 0,
        "slab_drain_updates returned while a writer held update_ref");
  __sync_fetch_and_sub(&wait_slab.update_ref, 1);
  for (int i = 0; i < 2000 && !atomic_load(&wait_done); i++) usleep(1000);
  check(atomic_load(&wait_done) == 1, "slab_drain_updates did not return");
  pthread_join(thread, NULL);
  /* Unlike freeze, a drain changes nothing on the slab. */
  check(atomic_load(&wait_slab.full) == 0 && atomic_load(&wait_slab.last_item) == 3,
        "slab_drain_updates modified the slab");
}

int main(void) {
  printf("== slab_freeze ==\n");
  test_rejects();
  test_drain();
  test_drain_updates();
  test_race();

  if (failures) {
    printf("== %lu failures ==\n", failures);
    return 1;
  }
  printf("== ok ==\n");
  return 0;
}
