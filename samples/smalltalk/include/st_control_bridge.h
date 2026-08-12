#ifndef ANVIL_SMALLTALK_CONTROL_BRIDGE_H
#define ANVIL_SMALLTALK_CONTROL_BRIDGE_H

#include "st_send_bridge.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generated activations allocate exactly this public runtime record on their
 * native stack.  It remains live from enter through the unique method
 * epilogue; neither the bridge nor the control runtime retains it afterward. */
#define ST_AOT_CONTROL_SCOPE_SIZE ((uint32_t)sizeof(st_control_scope_t))

st_control_status_t st_aot_control_scope_enter(
    StFrame *frame, st_control_scope_t *scope, uint32_t establish_home);

st_control_status_t st_aot_control_scope_leave(
    StFrame *frame, st_control_scope_t *scope, st_value_t normal_value,
    st_value_t *value_out);

/* pending_out is written as 0/1 so the generated ABI does not depend on the
 * host C representation of bool. */
st_control_status_t st_aot_control_pending(
    StFrame *frame, uint32_t *pending_out, st_value_t *value_out);

st_control_status_t st_aot_control_non_local_return(
    StFrame *frame, StHomeToken *target, st_value_t value);

/* ABI return type is StValue solely so IR can terminate a value-returning
 * block.  The implementation never returns. */
_Noreturn st_value_t st_aot_control_contract_violation(
    st_control_status_t status, StFrame *frame);

#ifdef __cplusplus
}
#endif

#endif
