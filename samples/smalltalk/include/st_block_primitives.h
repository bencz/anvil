#ifndef ANVIL_SMALLTALK_BLOCK_PRIMITIVES_H
#define ANVIL_SMALLTALK_BLOCK_PRIMITIVES_H

#include "st_closure_bridge.h"
#include "st_primitive.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_BLOCK_PRIMITIVE_ABI_VERSION UINT32_C(1)

typedef enum {
    ST_BLOCK_PRIMITIVE_OK = 0,
    ST_BLOCK_PRIMITIVE_ERR_INVALID_ARGUMENT,
    ST_BLOCK_PRIMITIVE_ERR_UNKNOWN_OPERATION,
    ST_BLOCK_PRIMITIVE_ERR_WRONG_METHOD_ARITY,
    ST_BLOCK_PRIMITIVE_ERR_INVALID_FRAME,
    ST_BLOCK_PRIMITIVE_ERR_INVALID_CONTEXT,
    ST_BLOCK_PRIMITIVE_ERR_INVALID_CLOSURE,
    ST_BLOCK_PRIMITIVE_ERR_WRONG_BLOCK_ARITY,
    ST_BLOCK_PRIMITIVE_ERR_INVALID_ARGUMENT_ARRAY,
    ST_BLOCK_PRIMITIVE_ERR_INVALID_VALUE,
    ST_BLOCK_PRIMITIVE_ERR_EXPECTED_BOOLEAN,
    ST_BLOCK_PRIMITIVE_ERR_OUT_OF_MEMORY,
    ST_BLOCK_PRIMITIVE_ERR_BLOCK_RETURNED,
    ST_BLOCK_PRIMITIVE_ERR_RUNTIME
} st_block_primitive_status_t;

const st_primitive_spec_t *st_block_primitive_specs(size_t *count_out);
const char *st_block_primitive_status_string(
    st_block_primitive_status_t status);

#ifdef __cplusplus
}
#endif

#endif
