#include "st_block_primitive_bridge.h"
#include "block_primitives_internal.h"

#define DEFINE_BRIDGE(name_, operation_)                                    \
    uint32_t name_(                                                          \
        StFrame *frame, st_value_t receiver,                                 \
        const st_value_t *arguments, size_t argument_count,                  \
        st_value_t *result_out, uint32_t *detail_out)                        \
    {                                                                        \
        return (uint32_t)st_block_primitive_execute_internal(                \
            frame, (operation_), receiver, arguments, argument_count,        \
            result_out, detail_out);                                         \
    }

DEFINE_BRIDGE(
    st_aot_block_value_primitive_execute,
    ST_BLOCK_OPERATION_VALUE)
DEFINE_BRIDGE(
    st_aot_block_value_primitive_1_execute,
    ST_BLOCK_OPERATION_VALUE_1)
DEFINE_BRIDGE(
    st_aot_block_value_primitive_2_execute,
    ST_BLOCK_OPERATION_VALUE_2)
DEFINE_BRIDGE(
    st_aot_block_value_primitive_3_execute,
    ST_BLOCK_OPERATION_VALUE_3)
DEFINE_BRIDGE(
    st_aot_block_value_arguments_primitive_execute,
    ST_BLOCK_OPERATION_VALUE_ARGUMENTS)
DEFINE_BRIDGE(
    st_aot_block_while_true_primitive_execute,
    ST_BLOCK_OPERATION_WHILE_TRUE)

#undef DEFINE_BRIDGE
