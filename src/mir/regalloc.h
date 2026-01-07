#ifndef ANVIL_REGALLOC_H
#define ANVIL_REGALLOC_H

#include "mir.h"
#include "liveness.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnvilRegAllocResult {
    int* vreg_to_preg;
    int* vreg_to_spill;
    int num_vregs;
    int num_spill_slots;
} AnvilRegAllocResult;

typedef struct AnvilRegAllocConfig {
    const int* available_regs;
    int num_available_regs;
    const int* available_fp_regs;
    int num_available_fp_regs;
    const int* callee_saved;
    int num_callee_saved;
    int stack_slot_size;
    const int* prealloc;
    int num_prealloc;
    const int* prealloc_fp;
    int num_prealloc_fp;
} AnvilRegAllocConfig;

AnvilRegAllocResult* anvil_regalloc_linear_scan(AnvilMFunc* func, AnvilRegAllocConfig* config);
void anvil_regalloc_apply(AnvilMFunc* func, AnvilRegAllocResult* result, AnvilRegAllocConfig* config);
void anvil_regalloc_result_free(AnvilRegAllocResult* result);

#ifdef __cplusplus
}
#endif

#endif
