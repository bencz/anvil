/* Internal contracts shared by the PowerPC backend components. */

#ifndef ANVIL_PPC_INTERNAL_H
#define ANVIL_PPC_INTERNAL_H

#include "anvil/anvil_ppc_mir.h"
#include "anvil/anvil_internal.h"
#include "anvil/anvil_analysis.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const anvil_ppc_target_desc_t *desc;
    const anvil_mir_func_t *mir;
    anvil_strbuf_t code;
    int *spill_offsets;
    size_t num_spill_offsets;
    int *frame_slot_offsets;
    size_t num_frame_slot_offsets;
    int gpr_save_offsets[32];
    int fpr_save_offsets[32];
    int outgoing_size;
    int frame_size;
    int fp_const_scratch_offset;
    size_t label_counter;
    bool has_frame;
    bool failed;
} ppc_mir_emit_t;

typedef struct anvil_ppc_abi_ops {
    const char *name;
    anvil_abi_t abi;
    anvil_syntax_t syntax;
    void (*emit_function_header)(ppc_mir_emit_t *emit);
    void (*emit_direct_call)(ppc_mir_emit_t *emit, const char *symbol);
    void (*emit_indirect_call)(ppc_mir_emit_t *emit, const char *target_reg);
} ppc_abi_ops_t;

extern const ppc_abi_ops_t ppc_elf32_abi_ops;
extern const ppc_abi_ops_t ppc_elfv1_abi_ops;
extern const ppc_abi_ops_t ppc_elfv2_abi_ops;

extern const int ppc_gpr_arg_regs[8];
extern const int ppc32_fpr_arg_regs[8];
extern const int ppc64_fpr_arg_regs[13];
extern const int ppc_alloc_gpr_regs[17];
extern const int ppc_alloc_fpr_regs[18];
extern const int ppc_scratch_gpr_regs[2];
extern const int ppc_scratch_fpr_regs[2];
extern const anvil_ppc_target_desc_t ppc32_target_desc;
extern const anvil_ppc_target_desc_t ppc64_target_desc;
extern const anvil_ppc_target_desc_t ppc64le_target_desc;

#endif
