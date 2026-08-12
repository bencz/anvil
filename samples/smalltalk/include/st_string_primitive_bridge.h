#ifndef ANVIL_SMALLTALK_STRING_PRIMITIVE_BRIDGE_H
#define ANVIL_SMALLTALK_STRING_PRIMITIVE_BRIDGE_H

#include "st_runtime.h"
#include "st_value.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Uniform runtime-symbol ABI consumed by the AOT primitive prologue. */
uint32_t st_aot_string_compare_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out);

uint32_t st_aot_string_concat_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out);

#ifdef __cplusplus
}
#endif

#endif
