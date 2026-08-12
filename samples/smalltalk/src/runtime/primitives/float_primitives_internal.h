#ifndef ANVIL_SMALLTALK_FLOAT_PRIMITIVES_INTERNAL_H
#define ANVIL_SMALLTALK_FLOAT_PRIMITIVES_INTERNAL_H

#include "st_float_primitives.h"

/* Runtime-private operation selectors.  They are deliberately absent from
 * the image primitive catalog and are never compiler intrinsic IDs. */
typedef enum {
    ST_FLOAT_OPERATION_EQUALS = 31,
    ST_FLOAT_OPERATION_LESS_THAN = 32,
    ST_FLOAT_OPERATION_GREATER_THAN = 33,
    ST_FLOAT_OPERATION_LESS_EQUALS = 34,
    ST_FLOAT_OPERATION_GREATER_EQUALS = 35,
    ST_FLOAT_OPERATION_ADD = 36,
    ST_FLOAT_OPERATION_SUBTRACT = 37,
    ST_FLOAT_OPERATION_MULTIPLY = 38,
    ST_FLOAT_OPERATION_DIVIDE = 39,
    ST_FLOAT_OPERATION_NEGATE = 40,
    ST_FLOAT_OPERATION_TRUNCATED = 41,
    ST_FLOAT_OPERATION_FLOOR = 42,
    ST_FLOAT_OPERATION_CEILING = 43,
    ST_FLOAT_OPERATION_ROUNDED = 44,
    ST_FLOAT_OPERATION_HASH = 45
} st_float_operation_t;

/* Failure is never encoded as an StValue. Arithmetic runs under a saved
 * FE_TONEAREST environment and restores all caller flags. NaNs propagate
 * left-first with payload/sign and are quieted; generated invalid-operation
 * NaNs are canonical. Comparisons are ordered and signed zeros compare equal.
 * The four integer conversions use toward-zero, floor, ceiling, and
 * nearest-ties-away. This representation-level executor returns
 * PROMOTION_REQUIRED outside tagged range; the AOT bridge continues exactly
 * once through st_integer_from_binary64_bits without retrying an allocation. */
st_float_primitive_status_t st_float_primitive_execute_internal(
    st_float_primitive_context_t *context, st_float_operation_t operation,
    st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out);

#endif
