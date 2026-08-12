#ifndef ANVIL_SMALLTALK_CONTROL_H
#define ANVIL_SMALLTALK_CONTROL_H

#include "st_runtime.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_CONTROL_ABI_VERSION UINT32_C(3)

typedef enum {
    ST_CONTROL_OK = 0,
    ST_CONTROL_ERR_INVALID_ARGUMENT,
    ST_CONTROL_ERR_INVALID_THREAD,
    ST_CONTROL_ERR_INVALID_FRAME,
    ST_CONTROL_ERR_INVALID_SCOPE,
    ST_CONTROL_ERR_INVALID_ENSURE,
    ST_CONTROL_ERR_INVALID_HANDLER,
    ST_CONTROL_ERR_INVALID_EXCEPTION,
    ST_CONTROL_ERR_INVALID_TOKEN,
    ST_CONTROL_ERR_OUT_OF_MEMORY,
    ST_CONTROL_ERR_ID_EXHAUSTED,
    ST_CONTROL_ERR_REFCOUNT_OVERFLOW,
    ST_CONTROL_ERR_BUSY,
    ST_CONTROL_ERR_BLOCK_RETURNED,
    ST_CONTROL_ERR_TARGET_NOT_ACTIVE,
    ST_CONTROL_ERR_REENTRANT,
    ST_CONTROL_ERR_CALLBACK_STATE,
    ST_CONTROL_ERR_VISITOR_ABORTED
} st_control_status_t;

typedef enum {
    ST_CONTROL_PENDING_NONE = 0,
    ST_CONTROL_PENDING_NON_LOCAL_RETURN,
    ST_CONTROL_PENDING_EXCEPTION
} st_control_pending_kind_t;

typedef enum {
    ST_CONTROL_LEAVE_NORMAL = 0,
    ST_CONTROL_LEAVE_NLR_PROPAGATE,
    ST_CONTROL_LEAVE_NLR_CAUGHT,
    ST_CONTROL_LEAVE_EXCEPTION_PROPAGATE
} st_control_leave_kind_t;

typedef struct {
    uint64_t thread_id;
    uint64_t activation_id;
} st_home_token_id_t;

typedef void *(*st_control_allocate_fn)(void *user, size_t size);
typedef void (*st_control_deallocate_fn)(void *user, void *pointer);

typedef struct {
    st_control_allocate_fn allocate;
    st_control_deallocate_fn deallocate;
    void *user;
} st_control_allocator_t;

typedef struct st_control_thread st_control_thread_t;
typedef struct st_control_scope st_control_scope_t;
typedef struct st_control_ensure st_control_ensure_t;
typedef struct st_control_handler st_control_handler_t;

typedef void (*st_control_ensure_fn)(
    void *user, StFrame *active_frame, st_control_thread_t *thread);
typedef bool (*st_control_root_visitor_fn)(
    void *user, const st_value_t *root_slot);
typedef bool (*st_control_class_object_fn)(
    void *user, st_value_t class_object, uint32_t *class_id_out);
typedef bool (*st_control_exception_match_fn)(
    void *user, uint32_t exception_class_id, uint32_t caught_class_id);

typedef struct {
    st_control_pending_kind_t kind;
    st_home_token_id_t target;
    st_value_t value;
    uint32_t exception_class_id;
    bool has_handler;
} st_control_pending_info_t;

typedef struct {
    st_control_leave_kind_t kind;
    st_value_t value;
} st_control_leave_result_t;

/* Public layouts permit allocation without a hidden factory. Fields prefixed
 * `_st_` are runtime-owned and must not be changed by clients. */
struct st_control_thread {
    uint32_t _st_abi_version;
    uint32_t _st_state;
    uint64_t _st_thread_id;
    uint64_t _st_next_activation_id;
    void *_st_frame_thread_identity;
    st_control_allocator_t _st_allocator;
    st_control_scope_t *_st_top_scope;
    StHomeToken *_st_pending_target;
    st_control_handler_t *_st_pending_handler;
    st_value_t _st_pending_value;
    uint32_t _st_pending_kind;
    uint32_t _st_pending_exception_class_id;
    uint32_t _st_callback_depth;
    st_control_ensure_t *_st_running_ensure;
    st_control_handler_t *_st_handler_top;
    st_control_class_object_fn _st_class_object;
    void *_st_class_object_user;
    _Atomic size_t _st_live_token_count;
};

struct st_control_scope {
    uint32_t _st_abi_version;
    uint32_t _st_state;
    st_control_thread_t *_st_thread;
    StFrame *_st_frame;
    st_control_scope_t *_st_previous;
    st_control_ensure_t *_st_ensure_top;
    StHomeToken *_st_frame_home_ref;
    StHomeToken *_st_activation_home;
    st_value_t _st_leave_value;
    bool _st_leave_value_active;
};

struct st_control_ensure {
    uint32_t _st_abi_version;
    uint32_t _st_state;
    st_control_scope_t *_st_scope;
    st_control_ensure_t *_st_previous;
    st_control_ensure_fn _st_callback;
    void *_st_user;
    const st_value_t *_st_roots;
    size_t _st_root_count;
    st_control_ensure_t *_st_running_previous;
};

struct st_control_handler {
    uint32_t _st_abi_version;
    uint32_t _st_state;
    st_control_thread_t *_st_thread;
    st_control_scope_t *_st_scope;
    st_control_handler_t *_st_previous;
    uint32_t _st_caught_class_id;
    uint32_t _st_reserved;
    const st_value_t *_st_roots;
    size_t _st_root_count;
};

/* `thread` must initially be zeroed. `thread_identity` is the exact pointer
 * stored in StFrame.thread (currently st_aot_thread_t *); the AOT thread's
 * immutable `control` link authenticates this sidecar for frame/GC bridges.
 * Control operations are single-owner. Token retain/release/query are the
 * only operations safe to call concurrently. The allocator and its user data
 * must outlive the thread and every token; destroy enforces zero live tokens. */
st_control_status_t st_control_thread_init(
    st_control_thread_t *thread, void *thread_identity,
    st_control_allocator_t allocator);
st_control_status_t st_control_thread_destroy(st_control_thread_t *thread);

/* Installs the image-specific class-object authenticator used by on:do:.
 * The callback must prove that class_object is a live class object belonging
 * to this image and return the represented instance-side class ID.  It is
 * configured once while the thread is quiescent and is never a process-wide
 * global. */
st_control_status_t st_control_exception_configure(
    st_control_thread_t *thread, st_control_class_object_fn class_object,
    void *user);
st_control_status_t st_control_exception_class_resolve(
    st_control_thread_t *thread, st_value_t class_object,
    uint32_t *class_id_out);

void st_control_scope_init(st_control_scope_t *scope);
st_control_status_t st_control_scope_enter(
    st_control_thread_t *thread, st_control_scope_t *scope, StFrame *frame);

/* Creates the stable home owned by this activation and installs it in
 * frame->home. `home_out` is borrowed; a closure must retain it before the
 * activation leaves. On failure no scope, frame, ID, or live-token count
 * changes. */
st_control_status_t st_control_scope_establish_home(
    st_control_thread_t *thread, st_control_scope_t *scope,
    StHomeToken **home_out);

/* Runs all armed ensures LIFO, each exactly once and while `scope->frame` is
 * still active. A callback may enter and completely leave nested scopes, but
 * cannot push onto or leave this scope reentrantly. An NLR requested by an
 * ensure supersedes the previous pending NLR. */
st_control_status_t st_control_scope_leave(
    st_control_thread_t *thread, st_control_scope_t *scope,
    st_value_t normal_value, st_control_leave_result_t *result_out);

void st_control_ensure_init(st_control_ensure_t *ensure_record);
st_control_status_t st_control_ensure_push(
    st_control_thread_t *thread, st_control_scope_t *scope,
    st_control_ensure_t *ensure_record, st_control_ensure_fn callback,
    void *user);

/* Roots borrowed from push until the callback has returned. Every vector slot
 * is an exact StValue root; callers use a zero-length vector when the opaque
 * callback state contains no managed values. The record remains visible to
 * st_control_visit_roots while RUNNING, including during nested callbacks. */
st_control_status_t st_control_ensure_push_with_roots(
    st_control_thread_t *thread, st_control_scope_t *scope,
    st_control_ensure_t *ensure_record, st_control_ensure_fn callback,
    void *user, const st_value_t *roots, size_t root_count);

/* Executes the exact top armed ensure immediately.  It uses the same
 * callback-state protocol as scope_leave, so pending NLR/exception state is
 * preserved unless the callback publishes a replacement. */
st_control_status_t st_control_ensure_run(
    st_control_thread_t *thread, st_control_scope_t *scope,
    st_control_ensure_t *ensure_record);

void st_control_handler_init(st_control_handler_t *handler);
st_control_status_t st_control_handler_push(
    st_control_thread_t *thread, st_control_scope_t *scope,
    st_control_handler_t *handler, uint32_t caught_class_id,
    const st_value_t *roots, size_t root_count);
st_control_status_t st_control_handler_pop(
    st_control_thread_t *thread, st_control_handler_t *handler);
st_control_status_t st_control_handler_is_target(
    const st_control_thread_t *thread,
    const st_control_handler_t *handler, bool *is_target_out);
st_control_status_t st_control_handler_consume_exception(
    st_control_thread_t *thread, st_control_handler_t *handler,
    st_value_t *exception_out);

/* Publishes an already authenticated exception object.  match is invoked
 * from innermost to outermost handler and must implement class/subclass
 * matching over immutable image descriptors.  A callback/ensure may replace
 * an older pending control transfer; ordinary code may not. */
st_control_status_t st_control_exception_signal(
    st_control_thread_t *thread, st_value_t exception,
    uint32_t exception_class_id, st_control_exception_match_fn match,
    void *match_user);

/* Publishes a pending NLR without transferring C control flow. The generated
 * AOT method returns `value`; every epilogue calls scope_leave and propagates
 * result.value until ST_CONTROL_LEAVE_NLR_CAUGHT reaches the home activation.
 * The target must be active in the current dynamic scope chain. */
st_control_status_t st_control_non_local_return(
    st_control_thread_t *thread, StHomeToken *target, st_value_t value);
st_control_status_t st_control_pending_get(
    const st_control_thread_t *thread, st_control_pending_info_t *info_out);

/* Fatal-recovery/teardown operation only. It is refused while scopes or
 * callbacks are active. Normal generated code consumes pending state through
 * scope_leave. */
st_control_status_t st_control_pending_clear(st_control_thread_t *thread);

/* Validates the complete sidecar/scope/ensure relationship against the
 * physical frame chain and enumerates, without allocation, pending NLR,
 * scope-leave and armed/running ensure roots. Visitor failure is reported as
 * VISITOR_ABORTED; visited_out is zero on every failure. */
st_control_status_t st_control_visit_roots(
    const st_control_thread_t *thread, const StFrame *top_frame,
    st_control_root_visitor_fn visitor, void *user, size_t *visited_out);

/* Every token operation requires an already-owned reference, except use of
 * the borrowed activation reference while its scope is active. retain cannot
 * resurrect a token whose last owned reference was released. */
st_control_status_t st_home_token_retain(StHomeToken *token);
void st_home_token_release(StHomeToken *token);
bool st_home_token_is_active(const StHomeToken *token);
st_control_status_t st_home_token_id(
    const StHomeToken *token, st_home_token_id_t *id_out);

size_t st_control_live_token_count(const st_control_thread_t *thread);
const char *st_control_status_string(st_control_status_t status);

#ifdef __cplusplus
}
#endif

#endif
