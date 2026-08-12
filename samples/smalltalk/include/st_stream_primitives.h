#ifndef ANVIL_SMALLTALK_STREAM_PRIMITIVES_H
#define ANVIL_SMALLTALK_STREAM_PRIMITIVES_H

#include "st_heap.h"
#include "st_primitive.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_STREAM_PRIMITIVE_ABI_VERSION UINT32_C(1)

typedef enum {
    ST_STREAM_PRIMITIVE_OK = 0,
    ST_STREAM_PRIMITIVE_ERR_INVALID_ARGUMENT,
    ST_STREAM_PRIMITIVE_ERR_WRONG_ARITY,
    ST_STREAM_PRIMITIVE_ERR_INVALID_VALUE,
    ST_STREAM_PRIMITIVE_ERR_TYPE_MISMATCH,
    ST_STREAM_PRIMITIVE_ERR_NOT_MEMBER,
    ST_STREAM_PRIMITIVE_ERR_DANGLING_REFERENCE,
    ST_STREAM_PRIMITIVE_ERR_INVALID_DESCRIPTOR,
    ST_STREAM_PRIMITIVE_ERR_BAD_OBJECT,
    ST_STREAM_PRIMITIVE_ERR_DESCRIPTOR_OUT_OF_RANGE,
    ST_STREAM_PRIMITIVE_ERR_COUNT_OUT_OF_RANGE,
    ST_STREAM_PRIMITIVE_ERR_WRITE_FAILED,
    ST_STREAM_PRIMITIVE_ERR_ZERO_PROGRESS,
    ST_STREAM_PRIMITIVE_ERR_WRITE_CONTRACT
} st_stream_primitive_status_t;

/* The callback returns the byte count, zero, or -1.  On -1 it must place a
 * positive errno-compatible error number in `os_error_out`.  Passing the OS
 * failure through this explicit channel avoids a hidden last-error global and
 * makes EINTR/short-write behavior independently testable. */
typedef int64_t (*st_stream_write_fn)(void *user, int descriptor,
                                      const void *bytes, size_t byte_count,
                                      int *os_error_out);

typedef struct st_stream_primitive_context {
    uint64_t abi_cookie;
    st_heap_t *heap;
    uint32_t string_class_id;
    uint32_t string_shape_id;
    st_stream_write_fn write_bytes;
    void *write_user;
} st_stream_primitive_context_t;

typedef struct {
    st_heap_t *heap;
    uint32_t string_class_id;
    uint32_t string_shape_id;
    /* NULL selects the platform write(2) adapter. */
    st_stream_write_fn write_bytes;
    void *write_user;
} st_stream_primitive_options_t;

/* No allocation is performed by this context: its heap and immutable image
 * descriptors have embedding/image lifetime.  The configured String shape
 * must belong to the exact concrete class, have no fixed words, and use
 * byte-indexed storage. */
st_stream_primitive_status_t st_stream_primitive_context_init(
    st_stream_primitive_context_t *context,
    const st_stream_primitive_options_t *options);
void st_stream_primitive_context_destroy(
    st_stream_primitive_context_t *context);

/* Canonical StreamWritePrimitive ABI. `arguments` are descriptor,
 * requested byte count, and String. Descriptor/count must be authenticated
 * SmallIntegers; the String must be an exact live heap allocation of the
 * configured concrete class and shape. On failure `result_out` remains
 * ST_VALUE_INVALID and `os_error_out` is nonzero only for a write failure.
 * Success returns the written count as a SmallInteger. */
st_stream_primitive_status_t st_stream_write_primitive_execute(
    st_stream_primitive_context_t *context, st_value_t receiver,
    const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, int *os_error_out);

const st_primitive_spec_t *st_stream_primitive_specs(size_t *count_out);
const char *st_stream_primitive_status_string(
    st_stream_primitive_status_t status);

#ifdef __cplusplus
}
#endif

#endif
