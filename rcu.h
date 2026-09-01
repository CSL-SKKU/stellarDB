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
    _Atomic(uint64_t) epoch;

    _Atomic(uint64_t) readers[2];

    /*
     * Only one writer / outstanding grace period.
     */
    _Atomic(bool) writer_busy;

    /*
     * -1: no grace period pending
     *  0/1: waiting for readers of this slot
     */
    _Atomic(int) pending_phase;

    rcu_callback_t callback;
    void *payload;
};

/*
 * RCU-managed pointer.
 *
 * Readers access ptr[their_phase].
 * Writer modifies ptr[next_phase].
 */
struct rcu_ptr {
    _Atomic(void *) ptr[2];
};

struct rcu_writer {
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