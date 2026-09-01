#include "rcu.h"

#include <assert.h>
#include <stddef.h>

#define RCU_NO_PHASE (-1)

#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax() __asm__ __volatile__("pause" ::: "memory")
#else
#define cpu_relax() __asm__ __volatile__("" ::: "memory")
#endif


/*
 * One TLS read-side state per thread.
 *
 * This implementation assumes a thread has at most one
 * active rcu_ctx read section at a time.
 *
 * If you need NUM_RCU independent contexts per thread,
 * change these into arrays indexed by your RCU ID.
 */
static _Thread_local struct rcu_ctx *tls_ctx;
static _Thread_local uint64_t tls_epoch;
static _Thread_local bool tls_inside;


/*
 * Try to complete the pending grace period.
 *
 * Called both by:
 *
 *   - writer after publishing
 *   - last old-generation reader
 */
static void
rcu_try_finish(
    struct rcu_ctx *ctx,
    unsigned phase)
{
    if (__atomic_load_n(
            &ctx->readers[phase],
            __ATOMIC_SEQ_CST) != 0)
        return;

    int expected = (int)phase;

    /*
     * Exactly one thread claims grace-period completion.
     */
    if (!__atomic_compare_exchange_n(
            &ctx->pending_phase,
            &expected,
            RCU_NO_PHASE,
            false,
            __ATOMIC_SEQ_CST,
            __ATOMIC_SEQ_CST))
        return;

    /*
     * Retire the old topology slot before allowing another writer to reuse
     * it. This keeps both slots identical between grace periods, so a later
     * unrelated update cannot expose stale pointer values.
     */
    struct rcu_ptr *ptr = ctx->updated_ptrs;
    unsigned current_phase = phase ^ 1U;

    while (ptr != NULL) {
        struct rcu_ptr *next = ptr->updated_next;
        void *value = __atomic_load_n(
            &ptr->ptr[current_phase],
            __ATOMIC_SEQ_CST);

        __atomic_store_n(
            &ptr->ptr[phase],
            value,
            __ATOMIC_SEQ_CST);
        ptr->updated_next = ptr;
        ptr = next;
    }
    ctx->updated_ptrs = NULL;

    /* callback/payload were installed before pending_phase became visible. */
    rcu_callback_t callback = ctx->callback;
    void *payload = ctx->payload;

    if (callback != NULL)
        callback(payload);

    /*
     * Do not allow another writer to reuse the other slot
     * until cleanup has completed.
     */
    __atomic_store_n(
        &ctx->writer_busy,
        false,
        __ATOMIC_SEQ_CST);
}


static void
rcu_drop_reader(
    struct rcu_ctx *ctx,
    unsigned phase)
{
    uint64_t old =
        __atomic_fetch_sub(
            &ctx->readers[phase],
            1,
            __ATOMIC_SEQ_CST);

    assert(old != 0);

    /*
     * We changed:
     *
     *     1 -> 0
     *
     * so we may be the thread completing the grace period.
     */
    if (old == 1)
        rcu_try_finish(ctx, phase);
}


void
rcu_init(struct rcu_ctx *ctx)
{
    __atomic_store_n(
        &ctx->epoch,
        0,
        __ATOMIC_SEQ_CST);

    __atomic_store_n(
        &ctx->readers[0],
        0,
        __ATOMIC_SEQ_CST);

    __atomic_store_n(
        &ctx->readers[1],
        0,
        __ATOMIC_SEQ_CST);

    __atomic_store_n(
        &ctx->writer_busy,
        false,
        __ATOMIC_SEQ_CST);

    __atomic_store_n(
        &ctx->pending_phase,
        RCU_NO_PHASE,
        __ATOMIC_SEQ_CST);

    ctx->callback = NULL;
    ctx->payload = NULL;
    ctx->updated_ptrs = NULL;
}


void
rcu_ptr_init(
    struct rcu_ptr *ptr,
    void *value)
{
    __atomic_store_n(
        &ptr->ptr[0],
        value,
        __ATOMIC_SEQ_CST);

    __atomic_store_n(
        &ptr->ptr[1],
        value,
        __ATOMIC_SEQ_CST);

    /* A self-link marks a pointer that is not in an update list. */
    ptr->updated_next = ptr;
}


void
rcu_read_in(struct rcu_ctx *ctx)
{
    assert(!tls_inside);

    for (;;) {
        uint64_t epoch =
            __atomic_load_n(
                &ctx->epoch,
                __ATOMIC_SEQ_CST);

        unsigned phase =
            (unsigned)(epoch & 1);

        /*
         * Register ourselves in the generation we observed.
         */
        __atomic_fetch_add(
            &ctx->readers[phase],
            1,
            __ATOMIC_SEQ_CST);

        /*
         * Writer may have published between:
         *
         *     load(epoch)
         *     readers[phase]++
         *
         * Never dereference an RCU pointer until this
         * validation succeeds.
         */
        if (__atomic_load_n(
                &ctx->epoch,
                __ATOMIC_SEQ_CST) == epoch) {
            tls_ctx = ctx;
            tls_epoch = epoch;
            tls_inside = true;
            return;
        }

        /*
         * We registered into a stale generation.
         * Undo it and retry.
         */
        rcu_drop_reader(ctx, phase);
    }
}


void
rcu_read_out(struct rcu_ctx *ctx)
{
    assert(tls_inside);
    assert(tls_ctx == ctx);

    unsigned phase =
        (unsigned)(tls_epoch & 1);

    tls_inside = false;
    tls_ctx = NULL;

    rcu_drop_reader(ctx, phase);
}


void *
rcu_read_ptr(
    struct rcu_ctx *ctx,
    struct rcu_ptr *ptr)
{
    assert(tls_inside);
    assert(tls_ctx == ctx);

    unsigned phase =
        (unsigned)(tls_epoch & 1);

    return __atomic_load_n(
        &ptr->ptr[phase],
        __ATOMIC_SEQ_CST);
}


void *
rcu_current_ptr(
    struct rcu_ctx *ctx,
    struct rcu_ptr *ptr)
{
    uint64_t epoch = __atomic_load_n(
        &ctx->epoch,
        __ATOMIC_SEQ_CST);

    return __atomic_load_n(
        &ptr->ptr[epoch & 1U],
        __ATOMIC_SEQ_CST);
}


struct rcu_writer
rcu_writer_in(struct rcu_ctx *ctx)
{
    bool expected;

    for (;;) {
        expected = false;

        if (__atomic_compare_exchange_n(
                &ctx->writer_busy,
                &expected,
                true,
                false,
                __ATOMIC_SEQ_CST,
                __ATOMIC_SEQ_CST))
            break;

        cpu_relax();
    }

    /*
     * writer_busy remains held throughout the previous
     * grace period, so this must be clear now.
     */
    assert(
        __atomic_load_n(
            &ctx->pending_phase,
            __ATOMIC_SEQ_CST)
        == RCU_NO_PHASE);

    uint64_t old_epoch =
        __atomic_load_n(
            &ctx->epoch,
            __ATOMIC_SEQ_CST);

    struct rcu_writer writer = {
        .ctx = ctx,
        .old_epoch = old_epoch,
        .new_epoch = old_epoch + 1,

        .old_phase =
            (unsigned)(old_epoch & 1),

        .new_phase =
            (unsigned)((old_epoch + 1) & 1),
    };

    return writer;
}


void *
rcu_writer_ptr(
    struct rcu_writer *writer,
    struct rcu_ptr *ptr)
{
    return __atomic_load_n(
        &ptr->ptr[writer->new_phase],
        __ATOMIC_SEQ_CST);
}


void
rcu_writer_set_ptr(
    struct rcu_writer *writer,
    struct rcu_ptr *ptr,
    void *value)
{
    assert(writer->ctx != NULL);

    if (ptr->updated_next == ptr) {
        ptr->updated_next = writer->ctx->updated_ptrs;
        writer->ctx->updated_ptrs = ptr;
    }

    __atomic_store_n(
        &ptr->ptr[writer->new_phase],
        value,
        __ATOMIC_SEQ_CST);
}


void
rcu_writer_abort(
    struct rcu_ctx *ctx,
    struct rcu_writer *writer)
{
    assert(writer->ctx == ctx);
    assert(__atomic_load_n(
               &ctx->writer_busy,
               __ATOMIC_SEQ_CST));
    assert(__atomic_load_n(
               &ctx->pending_phase,
               __ATOMIC_SEQ_CST) == RCU_NO_PHASE);

    struct rcu_ptr *ptr = ctx->updated_ptrs;

    while (ptr != NULL) {
        struct rcu_ptr *next = ptr->updated_next;
        void *value = __atomic_load_n(
            &ptr->ptr[writer->old_phase],
            __ATOMIC_SEQ_CST);

        __atomic_store_n(
            &ptr->ptr[writer->new_phase],
            value,
            __ATOMIC_SEQ_CST);
        ptr->updated_next = ptr;
        ptr = next;
    }
    ctx->updated_ptrs = NULL;
    writer->ctx = NULL;

    __atomic_store_n(
        &ctx->writer_busy,
        false,
        __ATOMIC_SEQ_CST);
}


void
rcu_writer_publish(
    struct rcu_ctx *ctx,
    struct rcu_writer *writer,
    rcu_callback_t callback,
    void *payload)
{
    assert(writer->ctx == ctx);
    /*
     * New-phase topology must already be completely built
     * before this function is called.
     */
    ctx->callback = callback;
    ctx->payload = payload;

    /*
     * Linearization point.
     *
     * Readers entering after this point use new_phase.
     */
    __atomic_store_n(
        &ctx->epoch,
        writer->new_epoch,
        __ATOMIC_SEQ_CST);

    /*
     * Old generation is now draining.
     */
    __atomic_store_n(
        &ctx->pending_phase,
        (int)writer->old_phase,
        __ATOMIC_SEQ_CST);

    /*
     * There is an important race:
     *
     *     publish epoch
     *     old last reader exits
     *     pending_phase = old_phase
     *
     * That reader cannot finish the grace period because
     * pending_phase was not installed yet.
     *
     * Therefore writer checks once after installing it.
     */
    rcu_try_finish(
        ctx,
        writer->old_phase);

    writer->ctx = NULL;
}
