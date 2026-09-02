/* SPDX-License-Identifier: GPL-2.0-only */
/* -*- linux-c -*- ------------------------------------------------------- *
 *
 *   Copyright (C) 1991, 1992 Linus Torvalds
 *   Copyright 2007 rPath, Inc. - All Rights Reserved
 *
 * ----------------------------------------------------------------------- */

#ifndef MEM_ITEM_H
#define MEM_ITEM_H

#include <stdatomic.h>

/*
 * Very simple bitops for the boot code.
 */
static inline unsigned char test_bit(int nr, const size_t *addr) {
  unsigned char v;
  const size_t *p = addr;

  asm("btl %2,%1; setc %0" : "=qm"(v) : "m"(*p), "Ir"(nr));
  return v;
}

static inline void set_bit(int nr, size_t *addr) {
  asm("btsl %1,%0" : "+m"(*(size_t *)addr) : "Ir"(nr));
}

//#define SET_INVAL(x) set_bit(63, &x)
//#define TEST_INVAL(x) test_bit(63, &x)
#define GET_SIDX(x) ((size_t)((uint32_t)(x) & ~(1u << 31)))

struct slab;
struct index_entry {  // This index entry could be made much smaller by, e.g.,
                      // have 64b for [slab_size, slab_idx] it is then easy to
                      // do size -> slab* given a slab context
  union {
    struct slab *slab;
    void *page;
  };
  union {
    size_t slab_idx;
    void *lru;
  };
};

struct tree_entry {  // This index entry could be made much smaller by, e.g.,
                     // have 64b for [slab_size, slab_idx] it is then easy to do
                     // size -> slab* given a slab context
  uint64_t key;
  uint64_t seq;
  /* Advisory routing metadata; rebalancing publishes it atomically. */
  _Atomic(uint64_t) level;
  struct slab *slab;
};

struct index_scan {
  uint64_t *hashes;
  struct index_entry *entries;
  size_t nb_entries;
};

struct tree_scan {
  uint64_t *hashes;
  struct tree_entry *entries;
  size_t nb_entries;
};

typedef struct index_entry index_entry_t;
typedef struct tree_entry tree_entry_t;

#endif
