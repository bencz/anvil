#ifndef ST_DISPATCH_H
#define ST_DISPATCH_H

#include "anvil/anvil.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct StMethodDescriptor StMethodDescriptor;
typedef struct StHomeToken StHomeToken;

/* Portable baseline frame for the 64-bit Smalltalk method ABI.  `roots` is a
 * shadow-root vector owned by this frame; `caller` forms the precise frame
 * chain used by safepoints and the initial non-moving collector. `method`
 * names immutable AOT metadata, `home` is the stable activation token used by
 * closures/non-local returns, and `safepoint_id` selects the method's static
 * source/root map. */
typedef struct StFrame {
    void *thread;
    struct StFrame *caller;
    const StMethodDescriptor *method;
    StHomeToken *home;
    uint64_t receiver;
    const uint64_t *argv;
    uint64_t *roots;
    uint32_t argc;
    uint32_t root_count;
    uint32_t safepoint_id;
    uint32_t flags;
} StFrame;

enum {
    ST_FRAME_THREAD_FIELD = 0,
    ST_FRAME_CALLER_FIELD = 1,
    ST_FRAME_METHOD_FIELD = 2,
    ST_FRAME_HOME_FIELD = 3,
    ST_FRAME_RECEIVER_FIELD = 4,
    ST_FRAME_ARGV_FIELD = 5,
    ST_FRAME_ROOTS_FIELD = 6,
    ST_FRAME_ARGC_FIELD = 7,
    ST_FRAME_ROOT_COUNT_FIELD = 8,
    ST_FRAME_SAFEPOINT_FIELD = 9,
    ST_FRAME_FLAGS_FIELD = 10,
    ST_DISPATCH_METHOD_COUNT = 2
};

typedef struct st_dispatch_kernel {
    anvil_module_t *module;
    anvil_type_t *frame_type;
    anvil_type_t *method_type;
    anvil_type_t *method_ptr_type;
    anvil_type_t *vtable_type;
    anvil_value_t *vtable;
    anvil_func_t *methods[ST_DISPATCH_METHOD_COUNT];
    anvil_func_t *dispatcher;
} st_dispatch_kernel_t;

/* Build a complete module for a 64-bit target.  Every method has the uniform
 * uint64_t method(StFrame *) signature.  Slot 0 returns receiver + argc; slot
 * 1 returns receiver + argv[0] when present (or receiver for an empty vector).
 * Valid slots load ptr<func> from the relocatable
 * VTable and call it indirectly. Invalid slots call the typed external runtime
 * hook `uint64_t st_dispatch_miss(StFrame *, uint32_t)` without aliasing. */
bool st_dispatch_kernel_build(anvil_ctx_t *ctx,
                              st_dispatch_kernel_t *kernel);

#ifdef __cplusplus
}
#endif

#endif
