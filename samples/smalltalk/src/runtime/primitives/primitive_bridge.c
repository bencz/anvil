#include "st_primitive_bridge.h"

#include <stdlib.h>

_Noreturn st_value_t st_aot_core_primitive_contract_violation(
    uint32_t intrinsic_id, st_core_primitive_status_t status,
    const StFrame *frame)
{
    (void)intrinsic_id;
    (void)status;
    (void)frame;
    abort();
}

_Noreturn st_value_t st_aot_runtime_primitive_contract_violation(
    uint32_t status, uint32_t detail, const StFrame *frame)
{
    (void)status;
    (void)detail;
    (void)frame;
    abort();
}
