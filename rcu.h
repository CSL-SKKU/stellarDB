#ifndef STELLAR_RCU_H
#define STELLAR_RCU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*rcu_callback_t)(void *payload);

struct rcu_ctx {
    /*
     * Monotonically increasing generation.
     * ptr slot = epoch & 1.
     */
    uint64_t epoch;

    uint64_t readers[2];

    /*
     * Only one writer / outstanding grace period.
     */
    bool writer_busy;

    /*
     * -1: no grace period pending
     *  0/1: waiting for readers of this slot
     */
    int pending_phase;

    rcu_callback_t callback;
    void *payload;

    /* Writer-owned list of pointers changed in the pending generation. */
    struct rcu_ptr *updated_ptrs;
};

/*
 * RCU-managed pointer.
 *
 * Readers access ptr[their_phase].
 * Writer modifies ptr[next_phase]. After the old readers drain, changed
 * pointers are copied back into the retired slot so both slots are identical
 * before the next writer starts.
 */
struct rcu_ptr {
    void *ptr[2];

    /* Intrusive writer-only bookkeeping; readers never touch these fields. */
    struct rcu_ptr *updated_next;
};

struct rcu_writer {
    struct rcu_ctx *ctx;

    uint64_t old_epoch;
    uint64_t new_epoch;

    unsigned old_phase;
    unsigned new_phase;
};


/*
 * Initialize context.
 */
void rcu_init(struct rcu_ctx *ctx);


/*
 * Reader API.
 *
 * Must be paired:
 *
 *     rcu_read_in(ctx);
 *     p = rcu_read_ptr(ctx, &ptr);
 *     ...
 *     rcu_read_out(ctx);
 *
 * Nested read sections are NOT supported.
 */
void rcu_read_in(struct rcu_ctx *ctx);
void rcu_read_out(struct rcu_ctx *ctx);

void *rcu_read_ptr(struct rcu_ctx *ctx, struct rcu_ptr *ptr);

/*
 * Read the currently published slot without registering an RCU reader.
 * The caller must externally prevent publication for the duration of every
 * traversal that uses this helper.
 */
void *rcu_current_ptr(struct rcu_ctx *ctx, struct rcu_ptr *ptr);


/*
 * Writer API.
 *
 * Only one writer is allowed at a time.
 *
 * writer_in() may spin until the previous grace period,
 * including its callback, has completed.
 */
struct rcu_writer rcu_writer_in(struct rcu_ctx *ctx);

/*
 * Read/write the inactive pointer slot being constructed.
 */
void *rcu_writer_ptr(struct rcu_writer *writer, struct rcu_ptr *ptr);

void rcu_writer_set_ptr(struct rcu_writer *writer, struct rcu_ptr *ptr, void *value);

/*
 * Discard an unpublished generation and release the writer. All pointers
 * changed with rcu_writer_set_ptr() are restored to the current generation.
 */
void rcu_writer_abort(struct rcu_ctx *ctx, struct rcu_writer *writer);


/*
 * Publish the new generation.
 *
 * callback(payload) runs after all readers of the old
 * generation have left.
 *
 * The callback may run either:
 *
 *   - inside rcu_writer_publish(), or
 *   - inside the last rcu_read_out().
 *
 * callback MUST NOT call rcu_writer_in() on the same ctx.
 */
void rcu_writer_publish(struct rcu_ctx *ctx,
                        struct rcu_writer *writer,
                        rcu_callback_t callback,
                        void *payload);

/*
 * Convenience initialization for a two-slot pointer.
 *
 * Both generations initially point to the same object.
 */
void rcu_ptr_init(struct rcu_ptr *ptr, void *value);

#endif
