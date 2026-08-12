#include "st_block_primitives.h"
#include "block_primitives_internal.h"

#include <limits.h>

#define RUNTIME_SPEC(name_, arity_, symbol_)                                \
    {                                                                        \
        (name_), sizeof(name_) - 1u, (arity_), ST_PRIMITIVE_INSTANCE_ONLY,  \
        ST_PRIMITIVE_FALL_THROUGH, ST_PRIMITIVE_RUNTIME_SYMBOL,             \
        ST_PRIMITIVE_INVALID_INTRINSIC_ID, (symbol_), sizeof(symbol_) - 1u   \
    }

static const st_primitive_spec_t block_specs[] = {
    RUNTIME_SPEC(
        "BlockValuePrimitive", 0u,
        "st_aot_block_value_primitive_execute"),
    RUNTIME_SPEC(
        "BlockValuePrimitive1", 1u,
        "st_aot_block_value_primitive_1_execute"),
    RUNTIME_SPEC(
        "BlockValuePrimitive2", 2u,
        "st_aot_block_value_primitive_2_execute"),
    RUNTIME_SPEC(
        "BlockValuePrimitive3", 3u,
        "st_aot_block_value_primitive_3_execute"),
    RUNTIME_SPEC(
        "BlockValueArgsPrimitive", 1u,
        "st_aot_block_value_arguments_primitive_execute"),
    RUNTIME_SPEC(
        "BlockWhileTruePrimitive", 1u,
        "st_aot_block_while_true_primitive_execute")
};

#undef RUNTIME_SPEC

static st_block_primitive_status_t map_closure_status(
    st_aot_closure_status_t status)
{
    switch (status) {
    case ST_AOT_CLOSURE_OK:
        return ST_BLOCK_PRIMITIVE_OK;
    case ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT:
        return ST_BLOCK_PRIMITIVE_ERR_INVALID_ARGUMENT;
    case ST_AOT_CLOSURE_ERR_INVALID_CONTEXT:
        return ST_BLOCK_PRIMITIVE_ERR_INVALID_CONTEXT;
    case ST_AOT_CLOSURE_ERR_INVALID_FRAME:
        return ST_BLOCK_PRIMITIVE_ERR_INVALID_FRAME;
    case ST_AOT_CLOSURE_ERR_INVALID_CLOSURE:
        return ST_BLOCK_PRIMITIVE_ERR_INVALID_CLOSURE;
    case ST_AOT_CLOSURE_ERR_WRONG_ARITY:
        return ST_BLOCK_PRIMITIVE_ERR_WRONG_BLOCK_ARITY;
    case ST_AOT_CLOSURE_ERR_INVALID_CAPTURE:
        return ST_BLOCK_PRIMITIVE_ERR_INVALID_VALUE;
    case ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY:
        return ST_BLOCK_PRIMITIVE_ERR_OUT_OF_MEMORY;
    case ST_AOT_CLOSURE_ERR_HOME_RETURNED:
    case ST_AOT_CLOSURE_ERR_BLOCK_RETURNED:
        return ST_BLOCK_PRIMITIVE_ERR_BLOCK_RETURNED;
    case ST_AOT_CLOSURE_ERR_INVALID_DESCRIPTOR:
    case ST_AOT_CLOSURE_ERR_HOME_REQUIRED:
    case ST_AOT_CLOSURE_ERR_UNSUPPORTED:
    case ST_AOT_CLOSURE_ERR_BUSY:
    case ST_AOT_CLOSURE_ERR_HEAP:
        return ST_BLOCK_PRIMITIVE_ERR_RUNTIME;
    }
    return ST_BLOCK_PRIMITIVE_ERR_RUNTIME;
}

static bool operation_arity(st_block_operation_t operation,
                            size_t *arity_out,
                            bool *argument_array_out)
{
    *argument_array_out = false;
    switch (operation) {
    case ST_BLOCK_OPERATION_VALUE:
        *arity_out = 0u;
        return true;
    case ST_BLOCK_OPERATION_VALUE_1:
        *arity_out = 1u;
        return true;
    case ST_BLOCK_OPERATION_VALUE_2:
        *arity_out = 2u;
        return true;
    case ST_BLOCK_OPERATION_VALUE_3:
        *arity_out = 3u;
        return true;
    case ST_BLOCK_OPERATION_VALUE_ARGUMENTS:
        *arity_out = 1u;
        *argument_array_out = true;
        return true;
    case ST_BLOCK_OPERATION_WHILE_TRUE:
        *arity_out = 1u;
        return true;
    default:
        *arity_out = 0u;
        return false;
    }
}

static st_block_primitive_status_t pending_after_invoke(
    StFrame *frame, bool *pending_out, st_value_t *value_out,
    uint32_t *detail_out)
{
    st_aot_thread_t *thread = frame->thread;
    st_control_pending_info_t pending;
    st_control_status_t status;

    *pending_out = false;
    *value_out = st_value_nil();
    if (thread->control == NULL) return ST_BLOCK_PRIMITIVE_OK;

    status = st_control_pending_get(thread->control, &pending);
    if (status != ST_CONTROL_OK) {
        *detail_out = (uint32_t)status;
        return ST_BLOCK_PRIMITIVE_ERR_RUNTIME;
    }
    if (pending.kind != ST_CONTROL_PENDING_NONE) {
        *pending_out = true;
        *value_out = pending.value;
    }
    return ST_BLOCK_PRIMITIVE_OK;
}

static st_block_primitive_status_t execute_while_true(
    StFrame *frame, st_value_t condition_block, st_value_t body_block,
    st_value_t *result_out, uint32_t *detail_out)
{
    for (;;) {
        st_value_t condition = ST_VALUE_INVALID;
        st_value_t pending_value = st_value_nil();
        bool pending = false;
        st_aot_closure_status_t closure_status = st_aot_closure_invoke(
            frame, condition_block, NULL, 0u, &condition);
        st_block_primitive_status_t status;

        if (closure_status != ST_AOT_CLOSURE_OK) {
            *detail_out = (uint32_t)closure_status;
            return map_closure_status(closure_status);
        }
        status = pending_after_invoke(
            frame, &pending, &pending_value, detail_out);
        if (status != ST_BLOCK_PRIMITIVE_OK) return status;
        if (pending) {
            *result_out = pending_value;
            return ST_BLOCK_PRIMITIVE_OK;
        }
        if (condition == st_value_false()) {
            *result_out = st_value_nil();
            return ST_BLOCK_PRIMITIVE_OK;
        }
        if (condition != st_value_true())
            return ST_BLOCK_PRIMITIVE_ERR_EXPECTED_BOOLEAN;

        st_value_t ignored = ST_VALUE_INVALID;
        closure_status = st_aot_closure_invoke(
            frame, body_block, NULL, 0u, &ignored);
        if (closure_status != ST_AOT_CLOSURE_OK) {
            *detail_out = (uint32_t)closure_status;
            return map_closure_status(closure_status);
        }
        status = pending_after_invoke(
            frame, &pending, &pending_value, detail_out);
        if (status != ST_BLOCK_PRIMITIVE_OK) return status;
        if (pending) {
            *result_out = pending_value;
            return ST_BLOCK_PRIMITIVE_OK;
        }
    }
}

st_block_primitive_status_t st_block_primitive_execute_internal(
    StFrame *frame, st_block_operation_t operation, st_value_t receiver,
    const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    const st_value_t *block_arguments = arguments;
    uint32_t block_argument_count;
    size_t expected_method_arity;
    bool argument_array;
    st_aot_closure_status_t closure_status;

    if (result_out != NULL) *result_out = (st_value_t)ST_VALUE_INVALID;
    if (detail_out != NULL) *detail_out = 0u;
    if (result_out == NULL || detail_out == NULL)
        return ST_BLOCK_PRIMITIVE_ERR_INVALID_ARGUMENT;
    if (!operation_arity(
            operation, &expected_method_arity, &argument_array))
        return ST_BLOCK_PRIMITIVE_ERR_UNKNOWN_OPERATION;
    if (argument_count != expected_method_arity)
        return ST_BLOCK_PRIMITIVE_ERR_WRONG_METHOD_ARITY;
    if (argument_count != 0u && arguments == NULL)
        return ST_BLOCK_PRIMITIVE_ERR_INVALID_ARGUMENT;
    if (frame == NULL
            || st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK)
        return ST_BLOCK_PRIMITIVE_ERR_INVALID_FRAME;
    if (frame->receiver != receiver)
        return ST_BLOCK_PRIMITIVE_ERR_INVALID_FRAME;

    if (operation == ST_BLOCK_OPERATION_WHILE_TRUE)
        return execute_while_true(
            frame, receiver, arguments[0], result_out, detail_out);

    if (argument_array) {
        closure_status = st_aot_closure_argument_array_view(
            frame, arguments[0], &block_arguments, &block_argument_count);
        if (closure_status != ST_AOT_CLOSURE_OK) {
            *detail_out = (uint32_t)closure_status;
            return closure_status == ST_AOT_CLOSURE_ERR_INVALID_CAPTURE
                ? ST_BLOCK_PRIMITIVE_ERR_INVALID_ARGUMENT_ARRAY
                : map_closure_status(closure_status);
        }
    } else {
        if (argument_count > UINT32_MAX)
            return ST_BLOCK_PRIMITIVE_ERR_WRONG_METHOD_ARITY;
        block_argument_count = (uint32_t)argument_count;
    }

    closure_status = st_aot_closure_invoke(
        frame, receiver, block_arguments, block_argument_count, result_out);
    if (closure_status != ST_AOT_CLOSURE_OK) {
        *result_out = (st_value_t)ST_VALUE_INVALID;
        *detail_out = (uint32_t)closure_status;
    }
    return map_closure_status(closure_status);
}

const st_primitive_spec_t *st_block_primitive_specs(size_t *count_out)
{
    if (count_out != NULL)
        *count_out = sizeof(block_specs) / sizeof(block_specs[0]);
    return block_specs;
}

const char *st_block_primitive_status_string(
    st_block_primitive_status_t status)
{
    switch (status) {
    case ST_BLOCK_PRIMITIVE_OK: return "ok";
    case ST_BLOCK_PRIMITIVE_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_BLOCK_PRIMITIVE_ERR_UNKNOWN_OPERATION: return "unknown operation";
    case ST_BLOCK_PRIMITIVE_ERR_WRONG_METHOD_ARITY:
        return "wrong primitive method arity";
    case ST_BLOCK_PRIMITIVE_ERR_INVALID_FRAME: return "invalid AOT frame";
    case ST_BLOCK_PRIMITIVE_ERR_INVALID_CONTEXT:
        return "invalid closure context";
    case ST_BLOCK_PRIMITIVE_ERR_INVALID_CLOSURE: return "invalid closure";
    case ST_BLOCK_PRIMITIVE_ERR_WRONG_BLOCK_ARITY:
        return "wrong block arity";
    case ST_BLOCK_PRIMITIVE_ERR_INVALID_ARGUMENT_ARRAY:
        return "invalid argument Array";
    case ST_BLOCK_PRIMITIVE_ERR_INVALID_VALUE: return "invalid StValue";
    case ST_BLOCK_PRIMITIVE_ERR_EXPECTED_BOOLEAN:
        return "condition block did not answer a Boolean";
    case ST_BLOCK_PRIMITIVE_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_BLOCK_PRIMITIVE_ERR_BLOCK_RETURNED:
        return "block home has returned";
    case ST_BLOCK_PRIMITIVE_ERR_RUNTIME: return "closure runtime failure";
    }
    return "invalid block primitive status";
}
