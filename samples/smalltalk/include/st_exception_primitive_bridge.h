#ifndef ANVIL_SMALLTALK_EXCEPTION_PRIMITIVE_BRIDGE_H
#define ANVIL_SMALLTALK_EXCEPTION_PRIMITIVE_BRIDGE_H

#include "st_exception_primitives.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST_EXCEPTION_PRIMITIVE_BRIDGE_DECLARATION(name_)                    \
    uint32_t name_(                                                          \
        StFrame *frame, st_value_t receiver,                                 \
        const st_value_t *arguments, size_t argument_count,                  \
        st_value_t *result_out, uint32_t *detail_out)

ST_EXCEPTION_PRIMITIVE_BRIDGE_DECLARATION(
    st_aot_exception_signal_primitive_execute);
ST_EXCEPTION_PRIMITIVE_BRIDGE_DECLARATION(
    st_aot_block_on_exception_primitive_execute);
ST_EXCEPTION_PRIMITIVE_BRIDGE_DECLARATION(
    st_aot_block_unwind_primitive_execute);

#undef ST_EXCEPTION_PRIMITIVE_BRIDGE_DECLARATION

#ifdef __cplusplus
}
#endif

#endif
