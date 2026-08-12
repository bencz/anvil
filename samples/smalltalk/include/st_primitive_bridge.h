#ifndef ANVIL_SMALLTALK_PRIMITIVE_BRIDGE_H
#define ANVIL_SMALLTALK_PRIMITIVE_BRIDGE_H

#include "st_core_primitives.h"
#include "st_dispatch.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generated code reaches this only when a CANNOT_FAIL primitive violates its
 * compiler/runtime contract.  It always aborts; StValue is the IR-level return
 * type used only because Anvil has no noreturn terminator yet. */
_Noreturn st_value_t st_aot_core_primitive_contract_violation(
    uint32_t intrinsic_id, st_core_primitive_status_t status,
    const StFrame *frame);

/* Uniform runtime-symbol counterpart.  status/detail remain domain-specific
 * integers so each runtime primitive family can preserve its complete error
 * taxonomy without expanding the generated ABI. */
_Noreturn st_value_t st_aot_runtime_primitive_contract_violation(
    uint32_t status, uint32_t detail, const StFrame *frame);

#ifdef __cplusplus
}
#endif

#endif
