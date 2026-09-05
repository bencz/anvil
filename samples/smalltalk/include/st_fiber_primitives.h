#ifndef ANVIL_SMALLTALK_FIBER_PRIMITIVES_H
#define ANVIL_SMALLTALK_FIBER_PRIMITIVES_H

#include "st_fiber.h"
#include "st_primitive.h"

const st_primitive_spec_t *st_fiber_primitive_specs(size_t *count_out);

uint32_t st_aot_fiber_spawn(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out);

uint32_t st_aot_fiber_yield(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out);

uint32_t st_aot_fiber_sleep(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out);

uint32_t st_aot_fiber_join(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out);

uint32_t st_aot_fiber_run(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out);

uint32_t st_aot_fiber_detach(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out);

uint32_t st_aot_fiber_collect(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out);

#endif
