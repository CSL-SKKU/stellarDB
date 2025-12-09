#ifndef OPTIONS_H
#define OPTIONS_H

#define DEBUG 0
#define PINNING 1
#define PATH "/scratch%lu/kvell/slab-%lu-%lu-%lu"

#define WITH_FILTER 0

/* In memory structures */
#define RBTREE 0
#define BTREE 1
#define TNT 2

#define MEMORY_INDEX TNT
#define PAGECACHE_INDEX BTREE

#define ASCEND 1
#define DESCEND 2
#define RANDOM 3
#define INSERT_MODE RANDOM

#define OLD_PERCENT 10.0
#define EPOCH 100000

/* Queue depth management */
#define QUEUE_DEPTH 64
// #define QUEUE_DEPTH 4
#define MAX_NB_PENDING_CALLBACKS_PER_WORKER (4 * QUEUE_DEPTH)
#define NEVER_EXCEED_QUEUE_DEPTH \
  1  // Never submit more than QUEUE_DEPTH IO requests simultaneously, otherwise
     // up to 2*MAX_NB_PENDING_CALLBACKS_PER_WORKER (very unlikely)
#define WAIT_A_BIT_FOR_MORE_IOS \
  0  // If we realize we don't have QUEUE_DEPTH IO pending when submitting IOs,
     // check again if new incoming requests have arrived. Boost performance a
     // tiny bit for zipfian workloads on AWS, but really not worthwhile


/* Page cache */
//#define PAGE_CACHE_SIZE (PAGE_SIZE * 20480)
//#define PAGE_CACHE_SIZE (PAGE_SIZE * 14417920) //55GB
//#define PAGE_CACHE_SIZE (PAGE_SIZE * 7864320) //30GB
//#define PAGE_CACHE_SIZE (PAGE_SIZE * 3932160) //15GB
//#define PAGE_CACHE_SIZE (PAGE_SIZE * 2621440) //10GB
//#define PAGE_CACHE_SIZE (PAGE_SIZE * 786432) //3GB

//#define PAGE_CACHE_SIZE (PAGE_SIZE * 8388608) //32GB
//#define PAGE_CACHE_SIZE (PAGE_SIZE * 4194304) //16GB
//#define PAGE_CACHE_SIZE (PAGE_SIZE * 2097152) //8GB
//#define PAGE_CACHE_SIZE (PAGE_SIZE * 1048576) //4GB
//#define PAGE_CACHE_SIZE (PAGE_SIZE * 524288) //2GB

//#define PAGE_CACHE_SIZE (PAGE_SIZE * 1835008) //7GB
//#define PAGE_CACHE_SIZE (PAGE_SIZE * 1572864) //6GB
//#define PAGE_CACHE_SIZE (PAGE_SIZE * 1310720) //5GB
//#define PAGE_CACHE_SIZE (PAGE_SIZE *  262144) //1GB

#ifndef SELECTED_BENCH
#define SELECTED_BENCH ycsb_c_zipfian
#endif

#ifndef SELECTED_PAGE_CACHE_SIZE
#define SELECTED_PAGE_CACHE_SIZE (PAGE_SIZE * 2097152) // Default 8GB
#endif

#define PAGE_CACHE_SIZE SELECTED_PAGE_CACHE_SIZE

#define MAX_PAGE_CACHE (PAGE_CACHE_SIZE / PAGE_SIZE)
#define MAX_SCAN_CACHE ((PAGE_CACHE_SIZE / PAGE_SIZE) / 10)

/* Free list */
#define FREELIST_IN_MEMORY_ITEMS \
  (256)  // We need enough to never have to read from disk

#define MAX_MEM 5 * 1024 * 1024 * 1024LU
#define BYTE_PER_KV 45
#define GC_START_TRSHLD 0.9
#define GC_END_TRSHLD 0.75

#define KV_SIZE 1024
#define MAX_FILE_SIZE 16384 * PAGE_SIZE
//#define REALKEY_FILE_PATH "/home/"


#endif
