#ifndef ANVIL_SMALLTALK_STREAM_PRIMITIVE_BRIDGE_H
#define ANVIL_SMALLTALK_STREAM_PRIMITIVE_BRIDGE_H

#include "st_send_bridge.h"
#include "st_stream_primitives.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Uniform generated-code bridge. The status and OS detail channels are
 * separate from the language result: result_out remains ST_VALUE_INVALID on
 * every failure, detail_out is nonzero only for an underlying OS write
 * failure, and no process-global stream context is consulted. */
uint32_t st_aot_stream_write_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out);

_Noreturn st_value_t st_aot_stream_primitive_contract_violation(
    uint32_t status, uint32_t detail, const StFrame *frame);

#ifdef __cplusplus
}
#endif

#endif
