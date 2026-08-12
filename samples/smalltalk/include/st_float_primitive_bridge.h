#ifndef ANVIL_SMALLTALK_FLOAT_PRIMITIVE_BRIDGE_H
#define ANVIL_SMALLTALK_FLOAT_PRIMITIVE_BRIDGE_H

#include "st_runtime.h"
#include "st_value.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Uniform runtime-symbol ABI consumed by the AOT primitive prologue. */
#define ST_DECLARE_FLOAT_BRIDGE(name_)                                      \
    uint32_t name_(                                                         \
        StFrame *frame, st_value_t receiver, const st_value_t *arguments,   \
        size_t argument_count, st_value_t *result_out,                      \
        uint32_t *detail_out)

ST_DECLARE_FLOAT_BRIDGE(st_aot_float_equals_primitive_execute);
ST_DECLARE_FLOAT_BRIDGE(st_aot_float_less_than_primitive_execute);
ST_DECLARE_FLOAT_BRIDGE(st_aot_float_greater_than_primitive_execute);
ST_DECLARE_FLOAT_BRIDGE(st_aot_float_less_equals_primitive_execute);
ST_DECLARE_FLOAT_BRIDGE(st_aot_float_greater_equals_primitive_execute);
ST_DECLARE_FLOAT_BRIDGE(st_aot_float_add_primitive_execute);
ST_DECLARE_FLOAT_BRIDGE(st_aot_float_subtract_primitive_execute);
ST_DECLARE_FLOAT_BRIDGE(st_aot_float_multiply_primitive_execute);
ST_DECLARE_FLOAT_BRIDGE(st_aot_float_divide_primitive_execute);
ST_DECLARE_FLOAT_BRIDGE(st_aot_float_negate_primitive_execute);
ST_DECLARE_FLOAT_BRIDGE(st_aot_float_truncated_primitive_execute);
ST_DECLARE_FLOAT_BRIDGE(st_aot_float_floor_primitive_execute);
ST_DECLARE_FLOAT_BRIDGE(st_aot_float_ceiling_primitive_execute);
ST_DECLARE_FLOAT_BRIDGE(st_aot_float_rounded_primitive_execute);
ST_DECLARE_FLOAT_BRIDGE(st_aot_float_hash_primitive_execute);

#undef ST_DECLARE_FLOAT_BRIDGE

#ifdef __cplusplus
}
#endif

#endif
