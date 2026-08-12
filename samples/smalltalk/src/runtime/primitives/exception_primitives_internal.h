#ifndef ANVIL_SMALLTALK_EXCEPTION_PRIMITIVES_INTERNAL_H
#define ANVIL_SMALLTALK_EXCEPTION_PRIMITIVES_INTERNAL_H

#include "st_exception_primitives.h"

typedef enum {
    ST_EXCEPTION_OPERATION_SIGNAL = 0,
    ST_EXCEPTION_OPERATION_ON_DO,
    ST_EXCEPTION_OPERATION_ENSURE
} st_exception_operation_t;

st_exception_primitive_status_t st_exception_primitive_execute_internal(
    StFrame *frame, st_exception_operation_t operation,
    st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out,
    uint32_t *detail_out);

#endif
