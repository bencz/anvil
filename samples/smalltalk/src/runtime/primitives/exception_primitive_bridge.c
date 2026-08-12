#include "st_exception_primitive_bridge.h"
#include "exception_primitives_internal.h"

#define DEFINE_BRIDGE(name_, operation_)                                    \
    uint32_t name_(                                                          \
        StFrame *frame, st_value_t receiver,                                 \
        const st_value_t *arguments, size_t argument_count,                  \
        st_value_t *result_out, uint32_t *detail_out)                        \
    {                                                                        \
        return (uint32_t)st_exception_primitive_execute_internal(            \
            frame, (operation_), receiver, arguments, argument_count,        \
            result_out, detail_out);                                         \
    }

DEFINE_BRIDGE(
    st_aot_exception_signal_primitive_execute,
    ST_EXCEPTION_OPERATION_SIGNAL)
DEFINE_BRIDGE(
    st_aot_block_on_exception_primitive_execute,
    ST_EXCEPTION_OPERATION_ON_DO)
DEFINE_BRIDGE(
    st_aot_block_unwind_primitive_execute,
    ST_EXCEPTION_OPERATION_ENSURE)

#undef DEFINE_BRIDGE
