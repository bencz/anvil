/*
 * ANVIL - Shared PowerPC MachineIR backend interface.
 *
 * PPC32, PPC64 big-endian, and PPC64 little-endian use one lowering,
 * allocation, and emission pipeline. Target descriptors hold the ABI facts
 * that differ between variants.
 */

#ifndef ANVIL_PPC_MIR_H
#define ANVIL_PPC_MIR_H

#include "anvil.h"
#include "anvil_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ANVIL_PPC_VARIANT_PPC32 = 0,
    ANVIL_PPC_VARIANT_PPC64,
    ANVIL_PPC_VARIANT_PPC64LE
} anvil_ppc_variant_t;

typedef struct {
    anvil_ppc_variant_t variant;
    anvil_arch_t arch;
    const char *name;
    unsigned word_size;
    bool little_endian;
    bool uses_function_descriptors;
    unsigned min_frame_size;
    unsigned lr_save_offset;
    unsigned toc_save_offset;
    unsigned outgoing_arg_offset;
    unsigned incoming_arg_offset;
    const int *gpr_arg_regs;
    size_t num_gpr_arg_regs;
    const int *fpr_arg_regs;
    size_t num_fpr_arg_regs;
    int gpr_return_reg;
    int fpr_return_reg;
    int indirect_call_reg;
    const int *alloc_gpr_regs;
    size_t num_alloc_gpr_regs;
    const int *alloc_fpr_regs;
    size_t num_alloc_fpr_regs;
    const int *scratch_gpr_regs;
    size_t num_scratch_gpr_regs;
    const int *scratch_fpr_regs;
    size_t num_scratch_fpr_regs;
} anvil_ppc_target_desc_t;

const anvil_ppc_target_desc_t *
anvil_ppc_get_target_desc(anvil_ppc_variant_t variant);

anvil_mir_func_t *anvil_ppc_lower_func_to_mir(anvil_func_t *func,
                                              anvil_ppc_variant_t variant);
bool anvil_ppc_verify_mir_legal(const anvil_mir_func_t *mir,
                                anvil_ppc_variant_t variant,
                                char *error,
                                size_t error_len);
bool anvil_ppc_regalloc_mir(anvil_mir_func_t *mir,
                            anvil_ppc_variant_t variant);
bool anvil_ppc_emit_mir(const anvil_mir_func_t *mir,
                        anvil_ppc_variant_t variant,
                        char **output,
                        size_t *len);

anvil_error_t anvil_ppc_codegen_func(anvil_backend_t *be,
                                     anvil_func_t *func,
                                     anvil_ppc_variant_t variant,
                                     char **output,
                                     size_t *len);
anvil_error_t anvil_ppc_codegen_module(anvil_backend_t *be,
                                       anvil_module_t *mod,
                                       anvil_ppc_variant_t variant,
                                       char **output,
                                       size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_PPC_MIR_H */
