CC=clang  #If you use GCC, add -fno-strict-aliasing to the CFLAGS because the Google BTree does weird stuff
#CFLAGS=-Wall -O0 -ggdb3
CFLAGS=-O2 -ggdb3 -Wall -I.

CXX=clang++
CXXFLAGS=${CFLAGS}

# 기본값 설정
BENCH ?= ycsb_c_zipfian
PAGE_CACHE_SIZE ?= "(PAGE_SIZE * 2097152)" # Default 8 GiB
STELLAR_DEBUG ?= 0
DISTRIBUTOR_HIGH_UTIL ?= 80
IO_WORKER_LOW_UTIL ?= 50
REBALANCE_THRESHOLD ?= 5.0

# 매크로 전달
CFLAGS += -DSELECTED_BENCH=$(BENCH) -DSELECTED_PAGE_CACHE_SIZE=$(PAGE_CACHE_SIZE) \
	-DDEBUG=$(STELLAR_DEBUG) \
	-DDISTRIBUTOR_HIGH_UTIL=$(DISTRIBUTOR_HIGH_UTIL) \
	-DIO_WORKER_LOW_UTIL=$(IO_WORKER_LOW_UTIL) \
	-DREBALANCE_THRESHOLD=$(REBALANCE_THRESHOLD)
ifneq ($(REALKEY_FILE_PATH),)
CFLAGS += -DREALKEY_FILE_PATH=\"$(REALKEY_FILE_PATH)\"
endif

LDLIBS=-lm -lpthread -lstdc++

INDEXES_OBJ=indexes/rbtree.o indexes/btree.o indexes/tnt_centree.o indexes/tnt_subtree.o indexes/tnt_balance.o indexes/tnt_prune.o
OTHERS_OBJ=config.o slab.o freelist.o ioengine.o pagecache.o stats.o random.o slabworker.o workload-common.o workload-ycsb.o workload-dbbench.o workload-bgwork.o workload-production.o workload-locality.o workload-latency.o utils.o rcu.o in-memory-index-tnt.o fsst.o db_bench.o ${INDEXES_OBJ}
MAIN_OBJ=main.o ${OTHERS_OBJ} 

.PHONY: all clean

all: makefile.dep main 

test: test/test_main test/test_reins test/test_rebalance test/test_rebalance_api \
	test/test_prune_links test/test_prune_freeze test/test_shy test/test_split_skip \
	test/test_async_stale test/test_report test/test_idle_invalidation

# Test observations and fault injection never enter normal benchmark objects.
TEST_OTHERS_OBJ=$(addprefix .build/test/,${OTHERS_OBJ})
TEST_CFLAGS=${CFLAGS} -DSTELLAR_TESTING=1
.build/test/%.o: %.c
	@mkdir -p $(dir $@)
	${CC} ${TEST_CFLAGS} -MMD -MP -c $< -o $@
.build/test/%.o: %.cc
	@mkdir -p $(dir $@)
	${CXX} ${TEST_CFLAGS} -MMD -MP -c $< -o $@
-include $(wildcard .build/test/*.d .build/test/indexes/*.d .build/test/test/*.d)

test/test_report: .build/test/test/report.o ${TEST_OTHERS_OBJ}
	${CC} $^ ${CFLAGS} ${LDLIBS} -o $@

test/test_idle_invalidation: .build/test/test/idle_invalidation.o ${TEST_OTHERS_OBJ}
	${CC} $^ ${CFLAGS} ${LDLIBS} -o $@

test/test_async_stale: .build/test/test/async_stale.o ${TEST_OTHERS_OBJ}
	${CC} .build/test/test/async_stale.o ${TEST_OTHERS_OBJ} ${CFLAGS} ${LDLIBS} -o test/test_async_stale

test/test_split_skip: .build/test/test/split_skip.o ${TEST_OTHERS_OBJ}
	${CC} .build/test/test/split_skip.o ${TEST_OTHERS_OBJ} ${CFLAGS} ${LDLIBS} -o test/test_split_skip

test/test_shy: .build/test/test/shy.o ${TEST_OTHERS_OBJ}
	${CC} .build/test/test/shy.o ${TEST_OTHERS_OBJ} ${CFLAGS} ${LDLIBS} -o test/test_shy

test/test_prune_links: .build/test/test/prune_links.o ${TEST_OTHERS_OBJ}
	${CC} .build/test/test/prune_links.o ${TEST_OTHERS_OBJ} ${CFLAGS} ${LDLIBS} -o test/test_prune_links

test/test_prune_freeze: .build/test/test/prune_freeze.o ${TEST_OTHERS_OBJ}
	${CC} .build/test/test/prune_freeze.o ${TEST_OTHERS_OBJ} ${CFLAGS} ${LDLIBS} -o test/test_prune_freeze

test/test_rebalance: .build/test/test/rebalance.o .build/test/indexes/tnt_balance.o .build/test/rcu.o
	${CC} .build/test/test/rebalance.o .build/test/indexes/tnt_balance.o .build/test/rcu.o ${CFLAGS} ${LDLIBS} -o test/test_rebalance

test/test_rebalance_api: .build/test/test/rebalance_api.o ${TEST_OTHERS_OBJ}
	${CC} .build/test/test/rebalance_api.o ${TEST_OTHERS_OBJ} ${CFLAGS} ${LDLIBS} -o test/test_rebalance_api

test/test_main: .build/test/test/main.o ${TEST_OTHERS_OBJ}
	${CC} .build/test/test/main.o ${TEST_OTHERS_OBJ} ${CFLAGS} ${LDLIBS} -o test/test_main

test/test_reins: .build/test/test/reinsert.o ${TEST_OTHERS_OBJ}
	${CC} .build/test/test/reinsert.o ${TEST_OTHERS_OBJ} ${CFLAGS} ${LDLIBS} -o test/test_reins

makefile.dep: *.[Cch] indexes/*.[ch] indexes/*.cc test/*.c
	for i in *.[Cc]; do ${CC} -MM "$${i}" ${CFLAGS}; done > $@
	for i in indexes/*.c; do ${CC} -MM "$${i}" -MT $${i%.c}.o ${CFLAGS}; done >> $@
	for i in indexes/*.cc; do ${CXX} -MM "$${i}" -MT $${i%.cc}.o ${CXXFLAGS}; done >> $@
	for i in test/*.c; do ${CC} -MM "$${i}" -MT $${i%.c}.o ${CFLAGS}; done >> $@
	#find ./ -type f \( -iname \*.c -o -iname \*.cc \) | parallel clang -MM "{}" -MT "{.}".o > makefile.dep #If you find that the lines above take too long...

ifneq ($(MAKECMDGOALS),clean)
-include makefile.dep
endif

main: $(MAIN_OBJ)

clean:
	rm -rf .build/test
	rm -f *.o indexes/*.o test/*.o main test/test_main test/test_reins \
		test/test_rebalance test/test_rebalance_api test/test_prune_links \
		test/test_prune_freeze test/test_shy test/test_split_skip test/test_async_stale test/test_report test/test_idle_invalidation makefile.dep
