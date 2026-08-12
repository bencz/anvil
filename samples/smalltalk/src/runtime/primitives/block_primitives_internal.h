#ifndef ANVIL_SMALLTALK_BLOCK_PRIMITIVES_INTERNAL_H
#define ANVIL_SMALLTALK_BLOCK_PRIMITIVES_INTERNAL_H

#include "st_block_primitives.h"

/* Private dispatch domain. These are not catalog intrinsic IDs: every public
 * primitive is registered honestly as a distinct runtime symbol. */
typedef enum {
    ST_BLOCK_OPERATION_VALUE = 0,
    ST_BLOCK_OPERATION_VALUE_1,
    ST_BLOCK_OPERATION_VALUE_2,
    ST_BLOCK_OPERATION_VALUE_3,
    ST_BLOCK_OPERATION_VALUE_ARGUMENTS,
    ST_BLOCK_OPERATION_WHILE_TRUE
} st_block_operation_t;

st_block_primitive_status_t st_block_primitive_execute_internal(
    StFrame *frame, st_block_operation_t operation, st_value_t receiver,
    const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out);

#endif
