#include "st_exception_primitives.h"
#include "exception_primitives_internal.h"

#define CONTROL_RUNTIME_SPEC(name_, arity_, failure_, symbol_)               \
    {                                                                        \
        (name_), sizeof(name_) - 1u, (arity_),                              \
        ST_PRIMITIVE_INSTANCE_ONLY, (failure_),                             \
        ST_PRIMITIVE_RUNTIME_CONTROL_SYMBOL,                                \
        ST_PRIMITIVE_INVALID_INTRINSIC_ID,                                  \
        (symbol_), sizeof(symbol_) - 1u                                     \
    }

static const st_primitive_spec_t exception_specs[] = {
    CONTROL_RUNTIME_SPEC(
        "ExceptionSignal", 0u, ST_PRIMITIVE_CANNOT_FAIL,
        "st_aot_exception_signal_primitive_execute"),
    CONTROL_RUNTIME_SPEC(
        "BlockOnExceptionPrimitive", 2u, ST_PRIMITIVE_FALL_THROUGH,
        "st_aot_block_on_exception_primitive_execute"),
    CONTROL_RUNTIME_SPEC(
        "BlockUnwindPrimitive", 1u, ST_PRIMITIVE_FALL_THROUGH,
        "st_aot_block_unwind_primitive_execute")
};

#undef CONTROL_RUNTIME_SPEC

typedef struct {
    StFrame *frame;
    st_value_t cleanup;
    st_value_t result;
    st_aot_closure_status_t status;
} ensure_callback_context_t;

static st_exception_primitive_status_t map_closure_status(
    st_aot_closure_status_t status,
    st_exception_primitive_status_t invalid_block_status)
{
    switch (status) {
    case ST_AOT_CLOSURE_OK:
        return ST_EXCEPTION_PRIMITIVE_OK;
    case ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT:
        return ST_EXCEPTION_PRIMITIVE_ERR_INVALID_ARGUMENT;
    case ST_AOT_CLOSURE_ERR_INVALID_CONTEXT:
        return ST_EXCEPTION_PRIMITIVE_ERR_INVALID_CONTEXT;
    case ST_AOT_CLOSURE_ERR_INVALID_FRAME:
        return ST_EXCEPTION_PRIMITIVE_ERR_INVALID_FRAME;
    case ST_AOT_CLOSURE_ERR_INVALID_CLOSURE:
    case ST_AOT_CLOSURE_ERR_INVALID_DESCRIPTOR:
    case ST_AOT_CLOSURE_ERR_INVALID_CAPTURE:
    case ST_AOT_CLOSURE_ERR_HOME_REQUIRED:
        return invalid_block_status;
    case ST_AOT_CLOSURE_ERR_WRONG_ARITY:
        return ST_EXCEPTION_PRIMITIVE_ERR_WRONG_BLOCK_ARITY;
    case ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY:
        return ST_EXCEPTION_PRIMITIVE_ERR_OUT_OF_MEMORY;
    case ST_AOT_CLOSURE_ERR_HOME_RETURNED:
    case ST_AOT_CLOSURE_ERR_BLOCK_RETURNED:
        return ST_EXCEPTION_PRIMITIVE_ERR_BLOCK_RETURNED;
    case ST_AOT_CLOSURE_ERR_UNSUPPORTED:
    case ST_AOT_CLOSURE_ERR_BUSY:
    case ST_AOT_CLOSURE_ERR_HEAP:
        return ST_EXCEPTION_PRIMITIVE_ERR_RUNTIME;
    }
    return ST_EXCEPTION_PRIMITIVE_ERR_RUNTIME;
}

static st_exception_primitive_status_t frame_context(
    StFrame *frame, st_value_t receiver, st_aot_thread_t **thread_out,
    st_control_thread_t **control_out, st_control_scope_t **scope_out)
{
    st_aot_thread_t *thread;
    st_control_thread_t *control;
    if (frame == NULL ||
        st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK ||
        frame->receiver != receiver)
        return ST_EXCEPTION_PRIMITIVE_ERR_INVALID_FRAME;
    thread = frame->thread;
    control = thread->control;
    if (control == NULL ||
        control->_st_abi_version != ST_CONTROL_ABI_VERSION ||
        control->_st_frame_thread_identity != thread ||
        control->_st_top_scope == NULL ||
        control->_st_top_scope->_st_frame != frame)
        return ST_EXCEPTION_PRIMITIVE_ERR_INVALID_CONTEXT;
    *thread_out = thread;
    *control_out = control;
    *scope_out = control->_st_top_scope;
    return ST_EXCEPTION_PRIMITIVE_OK;
}

static bool class_is_or_inherits(
    void *user, uint32_t exception_class_id, uint32_t caught_class_id)
{
    const st_runtime_descriptors_t *descriptors = user;
    uint32_t cursor = exception_class_id;
    size_t hops = 0u;
    while (cursor != 0u && hops++ < descriptors->class_count) {
        const StClassDescriptor *descriptor = st_runtime_class(
            descriptors, cursor);
        if (descriptor == NULL) return false;
        if (cursor == caught_class_id) return true;
        cursor = descriptor->superclass_id;
    }
    return false;
}

static st_exception_primitive_status_t resolve_block(
    StFrame *frame, st_value_t block, uint32_t arity,
    st_exception_primitive_status_t invalid_status,
    uint32_t *detail_out)
{
    st_aot_closure_target_t target;
    st_aot_closure_status_t status = st_aot_closure_resolve(
        frame, block, arity, &target);
    if (status != ST_AOT_CLOSURE_OK) {
        *detail_out = (uint32_t)status;
        return map_closure_status(status, invalid_status);
    }
    return ST_EXCEPTION_PRIMITIVE_OK;
}

static st_exception_primitive_status_t execute_signal(
    StFrame *frame, st_value_t receiver, st_value_t *result_out,
    uint32_t *detail_out)
{
    st_aot_thread_t *thread;
    st_control_thread_t *control;
    st_control_scope_t *scope;
    uint32_t class_id = 0u;
    st_exception_primitive_status_t status = frame_context(
        frame, receiver, &thread, &control, &scope);
    (void)scope;
    if (status != ST_EXCEPTION_PRIMITIVE_OK) return status;
    if (st_value_kind(receiver) != ST_VALUE_OBJECT ||
        thread->object_class == NULL ||
        !thread->object_class(
            thread->object_class_user, receiver, &class_id) ||
        class_id == 0u ||
        st_runtime_class(thread->lookup->descriptors, class_id) == NULL) {
        return ST_EXCEPTION_PRIMITIVE_ERR_INVALID_EXCEPTION;
    }
    st_control_status_t control_status = st_control_exception_signal(
        control, receiver, class_id, class_is_or_inherits,
        (void *)thread->lookup->descriptors);
    if (control_status != ST_CONTROL_OK) {
        *detail_out = (uint32_t)control_status;
        return ST_EXCEPTION_PRIMITIVE_ERR_CONTROL;
    }
    *result_out = receiver;
    return ST_EXCEPTION_PRIMITIVE_OK;
}

static st_exception_primitive_status_t execute_on_do(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    st_value_t *result_out, uint32_t *detail_out)
{
    st_aot_thread_t *thread;
    st_control_thread_t *control;
    st_control_scope_t *scope;
    st_exception_primitive_status_t status = frame_context(
        frame, receiver, &thread, &control, &scope);
    if (status != ST_EXCEPTION_PRIMITIVE_OK) return status;

    status = resolve_block(
        frame, receiver, 0u,
        ST_EXCEPTION_PRIMITIVE_ERR_INVALID_PROTECTED_BLOCK, detail_out);
    if (status != ST_EXCEPTION_PRIMITIVE_OK) return status;
    status = resolve_block(
        frame, arguments[1], 1u,
        ST_EXCEPTION_PRIMITIVE_ERR_INVALID_HANDLER_BLOCK, detail_out);
    if (status != ST_EXCEPTION_PRIMITIVE_OK) return status;

    uint32_t caught_class_id = 0u;
    st_control_status_t control_status =
        st_control_exception_class_resolve(
            control, arguments[0], &caught_class_id);
    if (control_status != ST_CONTROL_OK ||
        st_runtime_class(
            thread->lookup->descriptors, caught_class_id) == NULL) {
        *detail_out = (uint32_t)control_status;
        return ST_EXCEPTION_PRIMITIVE_ERR_INVALID_EXCEPTION_CLASS;
    }

    st_value_t roots[3] = { receiver, arguments[0], arguments[1] };
    st_control_handler_t handler;
    st_control_handler_init(&handler);
    control_status = st_control_handler_push(
        control, scope, &handler, caught_class_id, roots, 3u);
    if (control_status != ST_CONTROL_OK) {
        *detail_out = (uint32_t)control_status;
        return ST_EXCEPTION_PRIMITIVE_ERR_CONTROL;
    }

    st_aot_closure_status_t closure_status = st_aot_closure_invoke(
        frame, receiver, NULL, 0u, result_out);
    bool targeted = false;
    control_status = st_control_handler_is_target(
        control, &handler, &targeted);
    if (control_status != ST_CONTROL_OK) {
        *detail_out = (uint32_t)control_status;
        return ST_EXCEPTION_PRIMITIVE_ERR_CONTROL;
    }

    if (targeted) {
        st_value_t exception = (st_value_t)ST_VALUE_INVALID;
        control_status = st_control_handler_consume_exception(
            control, &handler, &exception);
        if (control_status != ST_CONTROL_OK) {
            *detail_out = (uint32_t)control_status;
            return ST_EXCEPTION_PRIMITIVE_ERR_CONTROL;
        }
        if (closure_status != ST_AOT_CLOSURE_OK) {
            *detail_out = (uint32_t)closure_status;
            return map_closure_status(
                closure_status,
                ST_EXCEPTION_PRIMITIVE_ERR_INVALID_PROTECTED_BLOCK);
        }
        closure_status = st_aot_closure_invoke(
            frame, arguments[1], &exception, 1u, result_out);
        if (closure_status != ST_AOT_CLOSURE_OK) {
            *detail_out = (uint32_t)closure_status;
            return map_closure_status(
                closure_status,
                ST_EXCEPTION_PRIMITIVE_ERR_INVALID_HANDLER_BLOCK);
        }
        return ST_EXCEPTION_PRIMITIVE_OK;
    }

    control_status = st_control_handler_pop(control, &handler);
    if (control_status != ST_CONTROL_OK) {
        *detail_out = (uint32_t)control_status;
        return ST_EXCEPTION_PRIMITIVE_ERR_CONTROL;
    }
    if (closure_status != ST_AOT_CLOSURE_OK) {
        *detail_out = (uint32_t)closure_status;
        return map_closure_status(
            closure_status,
            ST_EXCEPTION_PRIMITIVE_ERR_INVALID_PROTECTED_BLOCK);
    }
    return ST_EXCEPTION_PRIMITIVE_OK;
}

static void run_ensure_callback(
    void *user, StFrame *active_frame, st_control_thread_t *control)
{
    ensure_callback_context_t *context = user;
    (void)control;
    if (context == NULL || active_frame != context->frame) {
        if (context != NULL)
            context->status = ST_AOT_CLOSURE_ERR_INVALID_FRAME;
        return;
    }
    context->status = st_aot_closure_invoke(
        active_frame, context->cleanup, NULL, 0u, &context->result);
}

static st_exception_primitive_status_t execute_ensure(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    st_value_t *result_out, uint32_t *detail_out)
{
    st_aot_thread_t *thread;
    st_control_thread_t *control;
    st_control_scope_t *scope;
    st_exception_primitive_status_t status = frame_context(
        frame, receiver, &thread, &control, &scope);
    (void)thread;
    if (status != ST_EXCEPTION_PRIMITIVE_OK) return status;
    status = resolve_block(
        frame, receiver, 0u,
        ST_EXCEPTION_PRIMITIVE_ERR_INVALID_PROTECTED_BLOCK, detail_out);
    if (status != ST_EXCEPTION_PRIMITIVE_OK) return status;
    status = resolve_block(
        frame, arguments[0], 0u,
        ST_EXCEPTION_PRIMITIVE_ERR_INVALID_ENSURE_BLOCK, detail_out);
    if (status != ST_EXCEPTION_PRIMITIVE_OK) return status;

    st_value_t roots[2] = { receiver, arguments[0] };
    ensure_callback_context_t callback_context = {
        .frame = frame,
        .cleanup = arguments[0],
        .result = (st_value_t)ST_VALUE_INVALID,
        .status = ST_AOT_CLOSURE_OK
    };
    st_control_ensure_t ensure_record;
    st_control_ensure_init(&ensure_record);
    st_control_status_t control_status = st_control_ensure_push_with_roots(
        control, scope, &ensure_record, run_ensure_callback,
        &callback_context, roots, 2u);
    if (control_status != ST_CONTROL_OK) {
        *detail_out = (uint32_t)control_status;
        return ST_EXCEPTION_PRIMITIVE_ERR_CONTROL;
    }

    st_aot_closure_status_t protected_status = st_aot_closure_invoke(
        frame, receiver, NULL, 0u, result_out);
    control_status = st_control_ensure_run(
        control, scope, &ensure_record);
    if (control_status != ST_CONTROL_OK) {
        *detail_out = (uint32_t)control_status;
        return ST_EXCEPTION_PRIMITIVE_ERR_CONTROL;
    }
    if (callback_context.status != ST_AOT_CLOSURE_OK) {
        *detail_out = (uint32_t)callback_context.status;
        return map_closure_status(
            callback_context.status,
            ST_EXCEPTION_PRIMITIVE_ERR_INVALID_ENSURE_BLOCK);
    }
    if (protected_status != ST_AOT_CLOSURE_OK) {
        *detail_out = (uint32_t)protected_status;
        return map_closure_status(
            protected_status,
            ST_EXCEPTION_PRIMITIVE_ERR_INVALID_PROTECTED_BLOCK);
    }
    return ST_EXCEPTION_PRIMITIVE_OK;
}

st_exception_primitive_status_t st_exception_primitive_execute_internal(
    StFrame *frame, st_exception_operation_t operation,
    st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out,
    uint32_t *detail_out)
{
    size_t expected_arity;
    if (result_out != NULL) *result_out = (st_value_t)ST_VALUE_INVALID;
    if (detail_out != NULL) *detail_out = 0u;
    if (result_out == NULL || detail_out == NULL)
        return ST_EXCEPTION_PRIMITIVE_ERR_INVALID_ARGUMENT;
    switch (operation) {
    case ST_EXCEPTION_OPERATION_SIGNAL: expected_arity = 0u; break;
    case ST_EXCEPTION_OPERATION_ON_DO: expected_arity = 2u; break;
    case ST_EXCEPTION_OPERATION_ENSURE: expected_arity = 1u; break;
    default: return ST_EXCEPTION_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    if (argument_count != expected_arity)
        return ST_EXCEPTION_PRIMITIVE_ERR_WRONG_METHOD_ARITY;
    if (argument_count != 0u && arguments == NULL)
        return ST_EXCEPTION_PRIMITIVE_ERR_INVALID_ARGUMENT;
    switch (operation) {
    case ST_EXCEPTION_OPERATION_SIGNAL:
        return execute_signal(frame, receiver, result_out, detail_out);
    case ST_EXCEPTION_OPERATION_ON_DO:
        return execute_on_do(
            frame, receiver, arguments, result_out, detail_out);
    case ST_EXCEPTION_OPERATION_ENSURE:
        return execute_ensure(
            frame, receiver, arguments, result_out, detail_out);
    }
    return ST_EXCEPTION_PRIMITIVE_ERR_INVALID_ARGUMENT;
}

const st_primitive_spec_t *st_exception_primitive_specs(size_t *count_out)
{
    if (count_out != NULL)
        *count_out = sizeof(exception_specs) / sizeof(exception_specs[0]);
    return exception_specs;
}

const char *st_exception_primitive_status_string(
    st_exception_primitive_status_t status)
{
    switch (status) {
    case ST_EXCEPTION_PRIMITIVE_OK: return "ok";
    case ST_EXCEPTION_PRIMITIVE_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case ST_EXCEPTION_PRIMITIVE_ERR_WRONG_METHOD_ARITY:
        return "wrong primitive method arity";
    case ST_EXCEPTION_PRIMITIVE_ERR_INVALID_FRAME:
        return "invalid AOT frame";
    case ST_EXCEPTION_PRIMITIVE_ERR_INVALID_CONTEXT:
        return "invalid exception runtime context";
    case ST_EXCEPTION_PRIMITIVE_ERR_INVALID_EXCEPTION:
        return "invalid exception object";
    case ST_EXCEPTION_PRIMITIVE_ERR_INVALID_EXCEPTION_CLASS:
        return "invalid exception class object";
    case ST_EXCEPTION_PRIMITIVE_ERR_INVALID_PROTECTED_BLOCK:
        return "invalid protected block";
    case ST_EXCEPTION_PRIMITIVE_ERR_INVALID_HANDLER_BLOCK:
        return "invalid exception handler block";
    case ST_EXCEPTION_PRIMITIVE_ERR_INVALID_ENSURE_BLOCK:
        return "invalid ensure block";
    case ST_EXCEPTION_PRIMITIVE_ERR_WRONG_BLOCK_ARITY:
        return "wrong block arity";
    case ST_EXCEPTION_PRIMITIVE_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case ST_EXCEPTION_PRIMITIVE_ERR_BLOCK_RETURNED:
        return "block home has returned";
    case ST_EXCEPTION_PRIMITIVE_ERR_CONTROL:
        return "cooperative control failure";
    case ST_EXCEPTION_PRIMITIVE_ERR_RUNTIME:
        return "closure runtime failure";
    }
    return "invalid exception primitive status";
}
