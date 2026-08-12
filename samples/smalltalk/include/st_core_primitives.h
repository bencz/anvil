#ifndef ANVIL_SMALLTALK_CORE_PRIMITIVES_H
#define ANVIL_SMALLTALK_CORE_PRIMITIVES_H

#include "st_primitive.h"
#include "st_value.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stable IDs shared by the AOT lowering rules and the runtime fallback.
 * Zero is reserved by st_primitive.h and IDs must never be renumbered once an
 * image using this ABI has shipped.
 */
#define ST_CORE_PRIMITIVE_ABI_VERSION UINT32_C(1)

typedef enum {
    ST_INTRINSIC_IDENTITY = 1,
    ST_INTRINSIC_INT_EQUALS = 2,
    ST_INTRINSIC_INT_NOT_EQUALS = 3,
    ST_INTRINSIC_INT_LESS_THAN = 4,
    ST_INTRINSIC_INT_LESS_EQUALS = 5,
    ST_INTRINSIC_INT_GREATER_THAN = 6,
    ST_INTRINSIC_INT_GREATER_EQUALS = 7,
    ST_INTRINSIC_INT_ADD = 8,
    ST_INTRINSIC_INT_SUBTRACT = 9,
    ST_INTRINSIC_INT_MULTIPLY = 10,
    ST_INTRINSIC_INT_FLOOR_DIVIDE = 11,
    ST_INTRINSIC_INT_MODULO = 12,
    ST_INTRINSIC_INT_NEGATE = 13,
    ST_INTRINSIC_INT_BIT_AND = 14,
    ST_INTRINSIC_INT_BIT_OR = 15,
    ST_INTRINSIC_INT_BIT_XOR = 16,
    ST_INTRINSIC_INT_SHIFT = 17,
    ST_INTRINSIC_CHARACTER_NEW = 18,
    ST_INTRINSIC_CHARACTER_CODE = 19
} st_core_intrinsic_id_t;

/* Failure is never encoded as an StValue.  On every non-OK result with a
 * non-null output pointer the output word is cleared to ST_VALUE_INVALID
 * (zero), but callers must branch on the status rather than inspect that
 * word. */
typedef enum {
    ST_CORE_PRIMITIVE_OK = 0,
    ST_CORE_PRIMITIVE_ERR_INVALID_ARGUMENT,
    ST_CORE_PRIMITIVE_ERR_UNKNOWN_INTRINSIC,
    ST_CORE_PRIMITIVE_ERR_WRONG_ARITY,
    ST_CORE_PRIMITIVE_ERR_INVALID_VALUE,
    ST_CORE_PRIMITIVE_ERR_TYPE_MISMATCH,
    ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED,
    ST_CORE_PRIMITIVE_ERR_DIVISION_BY_ZERO,
    ST_CORE_PRIMITIVE_ERR_INVALID_CODE_POINT
} st_core_primitive_status_t;

/*
 * Executes the canonical semantics used by an unspecialized AOT fallback.
 * `argument_count` excludes the receiver.  Class-side dispatch has already
 * authenticated the receiver class before CHARACTER_NEW reaches this API;
 * this value-only layer deliberately does not dereference an unproven object.
 */
st_core_primitive_status_t st_core_primitive_execute(
    uint32_t intrinsic_id, st_value_t receiver,
    const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out);

/* Immutable catalog inputs for exactly the intrinsics implemented above.
 * The returned array has image/process lifetime and must not be modified. */
const st_primitive_spec_t *st_core_primitive_specs(size_t *count_out);

const char *st_core_primitive_status_string(st_core_primitive_status_t status);

#ifdef __cplusplus
}
#endif

#endif
