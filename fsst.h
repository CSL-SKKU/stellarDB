#ifndef FSST_H
#define FSST_H 1

tree_entry_t *pick_garbage_node();
void fsst_worker_init(void);
/* Final idle: stop producing copies at a slab boundary. Already issued copies
 * still drain through the I/O workers. A stopped worker is never restarted. */
void fsst_worker_request_stop(void);
int fsst_worker_stopped(void);

#endif
