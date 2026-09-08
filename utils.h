#ifndef UTILS_H
#define UTILS_H 1
#include "headers.h"
#include "stats.h"

#define PAGE_SIZE (4LU * 1024LU)
#define ONE_GB (1024 * 1024 * 1024)

#define die(msg, args...)                                                 \
  do {                                                                    \
    fprintf(stderr, "(%s,%d) " msg "\n", __FUNCTION__, __LINE__, ##args); \
    exit(-1);                                                             \
  } while (0)

#define perr(msg, args...)                                                \
  do {                                                                    \
    perror("Error: ");                                                    \
    fprintf(stderr, "(%s,%d) " msg "\n", __FUNCTION__, __LINE__, ##args); \
    exit(-1);                                                             \
  } while (0)

#ifdef __x86_64__
#define rdtscll(val)                                             \
  {                                                              \
    unsigned int __a, __d;                                       \
    asm volatile("rdtsc" : "=a"(__a), "=d"(__d));                \
    (val) = ((unsigned long)__a) | (((unsigned long)__d) << 32); \
  }
#else
#define rdtscll(val) __asm__ __volatile__("rdtsc" : "=A"(val))
#endif

#define NOP10()                                                              \
  asm("nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;" \
      "nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;" \
      "nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;" \
      "nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;" \
      "nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;" \
      "nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;" \
      "nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;" \
      "nop;")

#define maybe_unused __attribute__((unused))

#define STRINGIZE_(x) #x
#define STRINGIZE(x) STRINGIZE_(x)

/*
 * Cute timer macros
 * Usage:
 * declare_timer;
 * start_timer {
 *   ...
 * } stop_timer("Took %lu us", elapsed);
 */
#define declare_timer \
  uint64_t elapsed;   \
  struct timeval st, et;

#define start_timer gettimeofday(&st, NULL);

#define stop_timer(msg, args...)                                             \
  ;                                                                          \
  do {                                                                       \
    gettimeofday(&et, NULL);                                                 \
    elapsed =                                                                \
        ((et.tv_sec - st.tv_sec) * 1000000) + (et.tv_usec - st.tv_usec) + 1; \
    printf("(%s,%d) [%6lums] " msg "\n", __FUNCTION__, __LINE__,             \
           elapsed / 1000, ##args);                                          \
  } while (0)

/*
 * Cute debug timer.
 * declare_debug_timer;
 * start_debug_timer {
 *   ...
 * } stop_debug_timer(1000, "WARNING, this section took more than 1ms!");
 */
#define declare_debug_timer __attribute__((unused)) uint64_t __s, __e;

#if DEBUG

#define start_debug_timer rdtscll(__s);

#define stop_debug_timer(alert_thres, msg, args...)                           \
  ;                                                                           \
  do {                                                                        \
    rdtscll(__e);                                                             \
    uint64_t __elapsed = cycles_to_us(__e - __s);                             \
    if (__elapsed > (alert_thres))                                            \
      printf("(%s,%d) [%6luus] " msg "\n", __FUNCTION__, __LINE__, __elapsed, \
             ##args);                                                         \
  } while (0)

#else

#define start_debug_timer
#define stop_debug_timer(alert_thres, msg, args...)

#endif

/*
 * Foreach macro
 */
#define foreach(item, array)                                            \
  for (int keep = 1, count = 0, size = sizeof(array) / sizeof *(array); \
       keep && count != size; keep = !keep, count++)                    \
    for (item = *((array) + count); keep; keep = !keep)

/*
 * Cute way to print a message periodically in a loop.
 * declare_periodic_count;
 * for(...) {
 *    periodic_count(1000, "Hello"); // prints hello and the number of
 * iterations every second
 * }
 */
#define declare_periodic_count                            \
  uint64_t __real_start = 0, __start, __last, __nb_count; \
  if (!__real_start) {                                    \
    rdtscll(__real_start);                                \
    __start = __real_start;                               \
    __nb_count = 0;                                       \
  }

#define periodic_count(period, msg, args...)                                   \
  do {                                                                         \
    rdtscll(__last);                                                           \
    __nb_count++;                                                              \
    if (cycles_to_us(__last - __start) > ((period)*1000LU)) {                  \
      printf("(%s,%d) [%3lus] [%7lu ops/s] " msg "\n", __FUNCTION__, __LINE__, \
             cycles_to_us(__last - __real_start) / 1000000LU,                  \
             __nb_count * 1000000LU / cycles_to_us(__last - __start), ##args); \
      __nb_count = 0;                                                          \
      __start = __last;                                                        \
    }                                                                          \
  } while (0);

#endif

/*
 * Cute way to measure memory usage.
 */
#define declare_memory_counter             \
  struct rusage __memory;                  \
  uint64_t __previous_mem = 0, __used_mem; \
  getrusage(RUSAGE_SELF, &__memory);       \
  __previous_mem = __memory.ru_maxrss;

#define get_memory_usage(msg, args...)                                     \
  getrusage(RUSAGE_SELF, &__memory);                                       \
  __used_mem = __memory.ru_maxrss - __previous_mem;                        \
  __previous_mem = __memory.ru_maxrss;                                     \
  printf("(%s,%d) [%lu MB total - %lu MB since last measure] " msg "\n",   \
         __FUNCTION__, __LINE__, __previous_mem / 1024, __used_mem / 1024, \
         ##args);

/*
 * Busy waiting
 */
#define wait_for(cycles)            \
  do {                              \
    uint64_t _s, _e;                \
    rdtscll(_s);                    \
    while (1) {                     \
      rdtscll(_e);                  \
      if (_e - _s >= cycles) break; \
    }                               \
  } while (0);

/*
 * Helper functions
 */
uint64_t cycles_to_us(uint64_t cycles);
void shuffle(size_t *array, size_t n);
void shuffle_ranges(size_t *array, size_t n, size_t range_size);
void pin_me_on(int core);
