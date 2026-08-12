#ifndef ANVIL_SMALLTALK_HEAP_PRIMITIVE_BRIDGE_H
#define ANVIL_SMALLTALK_HEAP_PRIMITIVE_BRIDGE_H

#include "st_send_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

st_heap_primitive_status_t st_aot_heap_primitive_execute(
    StFrame *frame, uint32_t intrinsic_id, st_value_t receiver,
    const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out);

_Noreturn st_value_t st_aot_heap_primitive_contract_violation(
    uint32_t intrinsic_id, st_heap_primitive_status_t status,
    const StFrame *frame);

#ifdef __cplusplus
}
#endif

#endif
