#ifndef ANVIL_SMALLTALK_STRING_PRIMITIVES_H
#define ANVIL_SMALLTALK_STRING_PRIMITIVES_H

#include "st_heap.h"
#include "st_primitive.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_STRING_PRIMITIVE_ABI_VERSION UINT32_C(1)

typedef enum {
    ST_STRING_OPERATION_COMPARE = 1,
    ST_STRING_OPERATION_CONCAT = 2
} st_string_operation_t;

typedef enum {
    ST_STRING_PRIMITIVE_OK = 0,
    ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT,
    ST_STRING_PRIMITIVE_ERR_UNKNOWN_INTRINSIC,
    ST_STRING_PRIMITIVE_ERR_WRONG_ARITY,
    ST_STRING_PRIMITIVE_ERR_INVALID_VALUE,
    ST_STRING_PRIMITIVE_ERR_TYPE_MISMATCH,
    ST_STRING_PRIMITIVE_ERR_NOT_MEMBER,
    ST_STRING_PRIMITIVE_ERR_DANGLING_REFERENCE,
    ST_STRING_PRIMITIVE_ERR_INVALID_DESCRIPTOR,
    ST_STRING_PRIMITIVE_ERR_BAD_OBJECT,
    ST_STRING_PRIMITIVE_ERR_OUT_OF_MEMORY,
    ST_STRING_PRIMITIVE_ERR_OVERFLOW
} st_string_primitive_status_t;

typedef struct st_string_primitive_context {
    uint64_t abi_cookie;
    st_heap_t *heap;
    uint32_t string_class_id;
    uint32_t uint8_shape_id;
    uint32_t uint16_shape_id;
    uint32_t uint32_shape_id;
} st_string_primitive_context_t;

typedef struct {
    st_heap_t *heap;
    uint32_t string_class_id;
    uint32_t uint8_shape_id;
    uint32_t uint16_shape_id;
    uint32_t uint32_shape_id;
} st_string_primitive_options_t;

/* All three shapes must be distinct exact shapes of string_class_id, contain
 * no fixed words, and use respectively UINT8, UINT16 and UINT32 indexed
 * storage. No class or shape is inferred from a source-level name. */
st_string_primitive_status_t st_string_primitive_context_init(
    st_string_primitive_context_t *context,
    const st_string_primitive_options_t *options);
void st_string_primitive_context_destroy(
    st_string_primitive_context_t *context);

/* Shared checked arithmetic used by concatenation planning and image loaders.
 * `length_out` is cleared on every failure. */
bool st_string_primitive_combined_length(size_t left, size_t right,
                                         size_t *length_out);

/* Compare returns a tagged SmallInteger in {-1,0,1}. Concatenation scans both
 * operands as Unicode scalar sequences, selects the narrowest configured
 * representation, and publishes a new immutable exact String. On every error
 * result_out remains ST_VALUE_INVALID. */
st_string_primitive_status_t st_string_primitive_execute(
    st_string_primitive_context_t *context, st_string_operation_t operation,
    st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out);

const st_primitive_spec_t *st_string_primitive_specs(size_t *count_out);
/* Authenticate an indexed value array of Unicode Characters and create an
 * immutable String using the narrowest configured character representation. */
st_string_primitive_status_t st_string_primitive_from_characters(st_string_primitive_context_t *context, st_value_t array, st_value_t *result_out);

const char *st_string_primitive_status_string(
    st_string_primitive_status_t status);

#ifdef __cplusplus
}
#endif

#endif
