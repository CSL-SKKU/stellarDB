#ifndef RANDOM_H
#define RANDOM_H 1

typedef long (*random_gen_t)(void);

unsigned long xorshf96(void);
unsigned long locxorshf96(void);

void init_seed(void);  // must be called after each thread creation
void init_zipf_generator(long min, long max);
long zipf_next();     // zipf distribution, call init_zipf_generator first
long uniform_next();  // uniform, call init_zipf_generator first
long bogus_rand();    // returns something between 1 and 1000
long production_random1(void);  // production workload simulator
long production_random2(void);  // production workload simulator

const char *get_function_name(random_gen_t f);

#ifdef REALKEY_FILE_PATH
void load_real_keys(uint64_t nb_items_in_db);
uint64_t get_real_key(size_t position);
#endif

void cp_old_keys(size_t *prev, uint64_t nb_items_in_db);
void cp_old_keys_reverse(size_t *prev, uint64_t nb_items_in_db);
uint64_t get_old_key(size_t position);

#endif
