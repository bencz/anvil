#ifndef ANVIL_SMALLTALK_BLOCK_PRIMITIVE_BRIDGE_H
#define ANVIL_SMALLTALK_BLOCK_PRIMITIVE_BRIDGE_H

#include "st_block_primitives.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST_BLOCK_PRIMITIVE_BRIDGE_DECLARATION(name_)                        \
    uint32_t name_(                                                          \
        StFrame *frame, st_value_t receiver,                                 \
        const st_value_t *arguments, size_t argument_count,                  \
        st_value_t *result_out, uint32_t *detail_out)

ST_BLOCK_PRIMITIVE_BRIDGE_DECLARATION(
    st_aot_block_value_primitive_execute);
ST_BLOCK_PRIMITIVE_BRIDGE_DECLARATION(
    st_aot_block_value_primitive_1_execute);
ST_BLOCK_PRIMITIVE_BRIDGE_DECLARATION(
    st_aot_block_value_primitive_2_execute);
ST_BLOCK_PRIMITIVE_BRIDGE_DECLARATION(
    st_aot_block_value_primitive_3_execute);
ST_BLOCK_PRIMITIVE_BRIDGE_DECLARATION(
    st_aot_block_value_arguments_primitive_execute);
ST_BLOCK_PRIMITIVE_BRIDGE_DECLARATION(
    st_aot_block_while_true_primitive_execute);

#undef ST_BLOCK_PRIMITIVE_BRIDGE_DECLARATION

#ifdef __cplusplus
}
#endif

#endif
