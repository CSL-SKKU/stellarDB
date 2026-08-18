CC=clang  #If you use GCC, add -fno-strict-aliasing to the CFLAGS because the Google BTree does weird stuff
#CFLAGS=-Wall -O0 -ggdb3
CFLAGS=-O2 -ggdb3 -Wall -I.

CXX=clang++
CXXFLAGS=${CFLAGS}

# 기본값 설정
BENCH ?= ycsb_c_zipfian
PAGE_CACHE_SIZE ?= "(PAGE_SIZE * 2097152)" # Default 8 GiB

# 매크로 전달
CFLAGS += -DSELECTED_BENCH=$(BENCH) -DSELECTED_PAGE_CACHE_SIZE=$(PAGE_CACHE_SIZE)
ifneq ($(REALKEY_FILE_PATH),)
CFLAGS += -DREALKEY_FILE_PATH=\"$(REALKEY_FILE_PATH)\"
endif

LDLIBS=-lm -lpthread -lstdc++

INDEXES_OBJ=indexes/rbtree.o indexes/btree.o indexes/tnt_centree.o indexes/tnt_subtree.o indexes/tnt_balance.o
OTHERS_OBJ=config.o slab.o freelist.o ioengine.o pagecache.o stats.o random.o slabworker.o workload-common.o workload-ycsb.o workload-dbbench.o workload-bgwork.o workload-production.o workload-locality.o utils.o in-memory-index-tnt.o fsst.o db_bench.o ${INDEXES_OBJ}
MAIN_OBJ=main.o ${OTHERS_OBJ} 

.PHONY: all clean

all: makefile.dep main 

test: test/test_main test/test_reins

test/test_main: test/main.o ${OTHERS_OBJ}
	${CC} test/main.o ${OTHERS_OBJ} ${CFLAGS} ${LDLIBS} -o test/test_main

test/test_reins: test/reinsert.o ${OTHERS_OBJ}
	${CC} test/reinsert.o ${OTHERS_OBJ} ${CFLAGS} ${LDLIBS} -o test/test_reins

makefile.dep: *.[Cch] indexes/*.[ch] indexes/*.cc
	for i in *.[Cc]; do ${CC} -MM "$${i}" ${CFLAGS}; done > $@
	for i in indexes/*.c; do ${CC} -MM "$${i}" -MT $${i%.c}.o ${CFLAGS}; done >> $@
	for i in indexes/*.cc; do ${CXX} -MM "$${i}" -MT $${i%.cc}.o ${CXXFLAGS}; done >> $@
	#find ./ -type f \( -iname \*.c -o -iname \*.cc \) | parallel clang -MM "{}" -MT "{.}".o > makefile.dep #If you find that the lines above take too long...

-include makefile.dep

main: $(MAIN_OBJ)

clean:
	rm -f *.o indexes/*.o test/*.o main test/test_main test/test_reins makefile.dep
