#include "st_primitive_bridge.h"
#include "st_fatal.h"

#include <stdlib.h>

_Noreturn st_value_t st_aot_core_primitive_contract_violation(
    uint32_t intrinsic_id, st_core_primitive_status_t status,
    const StFrame *frame)
{
    (void)intrinsic_id;
    (void)frame;
    st_fatal_code("core primitive contract violation", "status", (uint64_t)status);
}

_Noreturn st_value_t st_aot_runtime_primitive_contract_violation(
    uint32_t status, uint32_t detail, const StFrame *frame)
{
    (void)frame;
    st_fatal_code("runtime primitive contract violation", "status", ((uint64_t)status << 32) | detail);
}
