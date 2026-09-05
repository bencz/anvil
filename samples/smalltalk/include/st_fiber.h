#ifndef ANVIL_SMALLTALK_FIBER_H
#define ANVIL_SMALLTALK_FIBER_H

#include "st_send_bridge.h"

typedef enum {
    ST_FIBER_OK = 0,
    ST_FIBER_INVALID_ARGUMENT,
    ST_FIBER_INVALID_FRAME,
    ST_FIBER_WRONG_THREAD,
    ST_FIBER_OUT_OF_MEMORY,
    ST_FIBER_INVALID_BLOCK,
    ST_FIBER_NOT_FOUND,
    ST_FIBER_DEADLOCK,
    ST_FIBER_BUSY,
    ST_FIBER_TIMEOUT,
    ST_FIBER_INTERRUPTED,
    ST_FIBER_EXCEPTION,
    ST_FIBER_RUNTIME_ERROR
} st_fiber_status_t;

typedef struct st_fiber_context st_fiber_context_t;

/* Cooperative, OS-thread-affine scheduler. IDs are monotonically increasing
 * within this context. Each fiber has an independent control thread; closures
 * with a non-local-return home cannot be used as a new fiber's entry point.
 * Join consumes the result, including an uncaught exception, exactly once.
 * Detached failures are delivered once by run/collect in the calling fiber;
 * detaching does not suppress unhandled exceptions.
 * Suspended roots are precise snapshots of live AOT maps and control roots.
 * The collector is non-moving; snapshots are discarded immediately on resume.
 * No native stack is scanned conservatively. */
st_fiber_status_t st_fiber_context_create(st_aot_thread_t *owner, size_t stack_size, st_fiber_context_t **context_out);
/* Close the socket context first. Live fibers reject shutdown without changes.
 * Once quiescent shutdown begins, results are released and new operations are
 * rejected. If escaped home tokens delay destruction, release their external
 * roots and retry destruction; the context remains valid for that retry. */
st_fiber_status_t st_fiber_context_destroy(st_fiber_context_t *context);
st_fiber_status_t st_fiber_spawn(StFrame *frame, st_value_t block, uint64_t *id_out);
st_fiber_status_t st_fiber_yield(StFrame *frame);
st_fiber_status_t st_fiber_sleep(StFrame *frame, uint32_t milliseconds);
st_fiber_status_t st_fiber_join(StFrame *frame, uint64_t id, st_value_t *result_out);
st_fiber_status_t st_fiber_run(StFrame *frame);
st_fiber_status_t st_fiber_detach(StFrame *frame, uint64_t id);
st_fiber_status_t st_fiber_collect(StFrame *frame, size_t *reclaimed_out);

#endif
