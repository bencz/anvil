#ifndef ANVIL_SMALLTALK_HEAP_PRIMITIVES_H
#define ANVIL_SMALLTALK_HEAP_PRIMITIVES_H

#include "st_core_primitives.h"
#include "st_heap.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ST_INTRINSIC_SIZE = 20,
    ST_INTRINSIC_AT = 21,
    ST_INTRINSIC_AT_PUT = 22,
    ST_INTRINSIC_INST_VAR_AT = 23,
    ST_INTRINSIC_INST_VAR_AT_PUT = 24,
    ST_INTRINSIC_BEHAVIOR_NEW = 25,
    ST_INTRINSIC_BEHAVIOR_NEW_SIZE = 26,
    ST_INTRINSIC_CLASS = 27,
    ST_INTRINSIC_HASH = 28,
    ST_INTRINSIC_ARRAY_EQUALS = 29,
    ST_INTRINSIC_STRING_HASH = 30
} st_heap_intrinsic_id_t;

typedef enum {
    ST_HEAP_INDEXED_ACCESS_NONE = 0,
    ST_HEAP_INDEXED_ACCESS_VALUES,
    ST_HEAP_INDEXED_ACCESS_UNSIGNED_INTEGER,
    ST_HEAP_INDEXED_ACCESS_CHARACTER
} st_heap_indexed_access_t;

typedef struct {
    uint32_t small_integer_class_id;
    uint32_t character_class_id;
    uint32_t nil_class_id;
    uint32_t false_class_id;
    uint32_t true_class_id;
} st_immediate_class_ids_t;

typedef enum {
    ST_HEAP_PRIMITIVE_OK = 0,
    ST_HEAP_PRIMITIVE_ERR_INVALID_ARGUMENT,
    ST_HEAP_PRIMITIVE_ERR_UNKNOWN_INTRINSIC,
    ST_HEAP_PRIMITIVE_ERR_WRONG_ARITY,
    ST_HEAP_PRIMITIVE_ERR_INVALID_VALUE,
    ST_HEAP_PRIMITIVE_ERR_TYPE_MISMATCH,
    ST_HEAP_PRIMITIVE_ERR_NOT_MEMBER,
    ST_HEAP_PRIMITIVE_ERR_DANGLING_REFERENCE,
    ST_HEAP_PRIMITIVE_ERR_INVALID_CLASS_MAP,
    ST_HEAP_PRIMITIVE_ERR_OUT_OF_MEMORY,
    ST_HEAP_PRIMITIVE_ERR_OVERFLOW,
    ST_HEAP_PRIMITIVE_ERR_PROMOTION_REQUIRED,
    ST_HEAP_PRIMITIVE_ERR_INDEX_OUT_OF_BOUNDS,
    ST_HEAP_PRIMITIVE_ERR_BAD_INDEXED_FORMAT,
    ST_HEAP_PRIMITIVE_ERR_BAD_SLOT_FORMAT,
    ST_HEAP_PRIMITIVE_ERR_VALUE_OUT_OF_RANGE,
    ST_HEAP_PRIMITIVE_ERR_IMMUTABLE,
    ST_HEAP_PRIMITIVE_ERR_ABSTRACT_CLASS,
    ST_HEAP_PRIMITIVE_ERR_BAD_OBJECT,
    ST_HEAP_PRIMITIVE_ERR_WRITE_BARRIER
} st_heap_primitive_status_t;

typedef struct st_heap_primitive_state st_heap_primitive_state_t;

typedef struct {
    st_heap_primitive_state_t *state;
} st_heap_primitive_context_t;

typedef struct {
    st_heap_t *heap;
    st_immediate_class_ids_t immediate_classes;

    /* Dense class-ID array: element zero is the class object representing ID
     * one. Every object is exact-base authenticated and its actual class must
     * equal the represented descriptor's metaclass_id. */
    const st_value_t *class_objects;
    size_t class_object_count;

    /* Dense shape-ID array. Physical integer formats deliberately require an
     * explicit language access policy, so String-like character storage is
     * never guessed from a class name. */
    const st_heap_indexed_access_t *indexed_access;
    size_t indexed_access_count;

    st_primitive_allocator_t allocator;
} st_heap_primitive_options_t;

/* `context` must be zero-initialized or previously destroyed. Initialization
 * is transactional: no partial mapping survives validation or OOM failure. */
st_heap_primitive_status_t st_heap_primitive_context_init(
    st_heap_primitive_context_t *context,
    const st_heap_primitive_options_t *options);
void st_heap_primitive_context_destroy(st_heap_primitive_context_t *context);

/* These are image roots owned by the context. The embedding must include
 * them in st_heap_collect's global root set for as long as the context lives. */
const st_value_t *st_heap_primitive_class_roots(
    const st_heap_primitive_context_t *context, size_t *count_out);

st_heap_primitive_status_t st_heap_primitive_execute(
    st_heap_primitive_context_t *context, uint32_t intrinsic_id,
    st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out);

const st_primitive_spec_t *st_heap_primitive_specs(size_t *count_out);
const char *st_heap_primitive_status_string(st_heap_primitive_status_t status);

#ifdef __cplusplus
}
#endif

#endif
