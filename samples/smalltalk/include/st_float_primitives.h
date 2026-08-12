#ifndef ANVIL_SMALLTALK_FLOAT_PRIMITIVES_H
#define ANVIL_SMALLTALK_FLOAT_PRIMITIVES_H

#include "st_heap.h"
#include "st_primitive.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ST_FLOAT_PRIMITIVE_OK = 0,
    ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT,
    ST_FLOAT_PRIMITIVE_ERR_UNKNOWN_OPERATION,
    ST_FLOAT_PRIMITIVE_ERR_WRONG_ARITY,
    ST_FLOAT_PRIMITIVE_ERR_INVALID_VALUE,
    ST_FLOAT_PRIMITIVE_ERR_TYPE_MISMATCH,
    ST_FLOAT_PRIMITIVE_ERR_NOT_MEMBER,
    ST_FLOAT_PRIMITIVE_ERR_DANGLING_REFERENCE,
    ST_FLOAT_PRIMITIVE_ERR_INVALID_DESCRIPTOR,
    ST_FLOAT_PRIMITIVE_ERR_BAD_OBJECT,
    ST_FLOAT_PRIMITIVE_ERR_OUT_OF_MEMORY,
    ST_FLOAT_PRIMITIVE_ERR_OVERFLOW,
    ST_FLOAT_PRIMITIVE_ERR_PROMOTION_REQUIRED,
    ST_FLOAT_PRIMITIVE_ERR_NON_FINITE,
    ST_FLOAT_PRIMITIVE_ERR_FLOAT_ENVIRONMENT
} st_float_primitive_status_t;

typedef struct st_float_primitive_state st_float_primitive_state_t;

typedef struct {
    st_float_primitive_state_t *state;
} st_float_primitive_context_t;

typedef struct {
    st_heap_t *heap;
    uint32_t boxed_float64_class_id;
    uint32_t boxed_float64_shape_id;
    st_primitive_allocator_t allocator;
} st_float_primitive_options_t;

/* The selected image shape is part of the ABI: one fixed word whose canonical
 * pointer-bitmap bit is clear, no indexed storage, and exact ownership by the
 * concrete BoxedFloat64 class. Initialization copies no descriptor data and
 * is transactional. */
st_float_primitive_status_t st_float_primitive_context_init(
    st_float_primitive_context_t *context,
    const st_float_primitive_options_t *options);
void st_float_primitive_context_destroy(st_float_primitive_context_t *context);

/* Borrowed heap identity used when composing the integer/float numeric
 * sidecar. NULL means the context is not live. */
st_heap_t *st_float_primitive_context_heap(
    st_float_primitive_context_t *context);
const st_heap_t *st_float_primitive_context_heap_const(
    const st_float_primitive_context_t *context);

/* Raw-bit helpers are the representation authority.  They preserve every
 * binary64 pattern exactly, including signed zero and signaling-NaN payloads.
 * All object reads authenticate an exact live heap base before dereference.
 * A heap which accepts allocation under the validated immutable descriptors
 * and then cannot authenticate that new object has violated an internal heap
 * invariant; box_bits terminates instead of publishing a recoverable error
 * while leaving a partially constructed registry entry. */
st_float_primitive_status_t st_float_primitive_box_bits(
    st_float_primitive_context_t *context, uint64_t ieee_binary64_bits,
    st_value_t *result_out);
st_float_primitive_status_t st_float_primitive_unbox_bits(
    st_float_primitive_context_t *context, st_value_t value,
    uint64_t *ieee_binary64_bits_out);

/* The image catalog publishes honest runtime symbols so every target emits
 * an ordinary relocation to the AOT bridge. Float equality is intentionally
 * Float-only; mixed Integer operands are coerced to binary64 only by ordered
 * comparison and arithmetic fallbacks. The bridge publishes every finite
 * rounded conversion as a canonical SmallInteger or LargeInteger through the
 * thread's numeric context. NaN and infinities cannot convert to Integer. */
const st_primitive_spec_t *st_float_primitive_specs(size_t *count_out);
const char *st_float_primitive_status_string(
    st_float_primitive_status_t status);

#ifdef __cplusplus
}
#endif

#endif
