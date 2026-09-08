#include "headers.h"
#include "db_bench.h"

/* Retain the serial-read workload through the ordinary benchmark interface.
 * Its latch and measurement context are owned entirely by the harness. */
struct serial_read {
  pthread_mutex_t lock;
  pthread_cond_t done;
  int completed;
};
static void serial_done(struct slab_callback *cb, void *item) {
  struct serial_read *s = bench_get_data(cb);
  compute_stats(cb, item);
  pthread_mutex_lock(&s->lock);
  s->completed = 1;
  pthread_cond_signal(&s->done);
  pthread_mutex_unlock(&s->lock);
}
static char *create_item(uint64_t key, uint64_t maximum) {
  (void)maximum;
#ifdef REALKEY_FILE_PATH
  key = get_real_key(key);
#endif
  return create_unique_item(cfg.kv_size, key);
}
static void initialize(struct workload *w, bench_t b) { (void)w; (void)b; init_rand(); }
static void launch(struct workload *w, bench_t b) {
  (void)b;
  struct serial_read s = {.lock = PTHREAD_MUTEX_INITIALIZER, .done = PTHREAD_COND_INITIALIZER};
  for (uint64_t i = 0; i < w->nb_requests_per_thread; i++) {
    struct slab_callback *cb = bench_cb();
    cb->item = create_item(uniform_next() % w->nb_items_in_db, w->nb_items_in_db);
    cb->cb = serial_done;
    bench_set_data(cb, &s);
    pthread_mutex_lock(&s.lock);
    s.completed = 0;
    bench_read(cb);
    while (!s.completed) pthread_cond_wait(&s.done, &s.lock);
    pthread_mutex_unlock(&s.lock);
  }
  pthread_cond_destroy(&s.done);
  pthread_mutex_destroy(&s.lock);
}
static int handles(bench_t b) { return b == latprobe; }
static const char *name(bench_t b) { (void)b; return "Serial reads"; }
static const char *api_name(void) { return "LATPROBE"; }
struct workload_api LATPROBE = {
  .init = initialize, .handles = handles, .launch = launch,
  .name = name, .api_name = api_name, .create_unique_item = create_item
};
