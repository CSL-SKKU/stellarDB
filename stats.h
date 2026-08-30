#ifndef STATS_H
#define STATS_H 1

struct slab_callback;

enum timing_stage {
  TIMING_STAGE_INVALID = -1,
  TIMING_STAGE_REQUEST_ENQUEUED = 0,
  TIMING_STAGE_REQUEST_DEQUEUED,
  TIMING_STAGE_LEAF_FOUND,
  TIMING_STAGE_INDEX_LOOKUP_DONE,
  TIMING_STAGE_STORAGE_TARGET_READY,
  TIMING_STAGE_IO_SUBMIT,
  TIMING_STAGE_IO_COMPLETE,
  TIMING_STAGE_NEW_INDEX_PUBLISHED,
  TIMING_STAGE_OLD_INDEX_INVALIDATED,
  TIMING_STAGE_REQUEST_COMPLETE,
};

void add_timing_stat(uint64_t elapsed);
void print_stats(void);

uint64_t cycles_to_us(uint64_t cycles);

void *allocate_payload(void);
void free_payload(struct slab_callback *c);
void add_time_in_payload(struct slab_callback *c, enum timing_stage origin);
uint64_t get_time_from_payload(struct slab_callback *c, size_t pos);
uint64_t get_origin_from_payload(struct slab_callback *c, size_t pos);
#endif
