/*
 * ANVIL - PowerPC 64-bit big-endian backend wrapper.
 *
 * The implementation lives in the shared PowerPC MachineIR backend. This file
 * only exposes the PPC64 ELFv1 target identity and selects the PPC64 descriptor.
 */

#include "../ppc_internal.h"

static const anvil_arch_info_t ppc64_arch_info = {
    .arch = ANVIL_ARCH_PPC64,
    .name = "PowerPC 64-bit",
    .ptr_size = 8,
    .addr_bits = 64,
    .word_size = 8,
    .num_gpr = 32,
    .num_fpr = 32,
    .endian = ANVIL_ENDIAN_BIG,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false,
};

const anvil_ppc_target_desc_t ppc64_target_desc = {
    .variant = ANVIL_PPC_VARIANT_PPC64,
    .arch = ANVIL_ARCH_PPC64,
    .name = "ppc64",
    .word_size = 8,
    .little_endian = false,
    .uses_function_descriptors = true,
    .min_frame_size = 112,
    .lr_save_offset = 16,
    .toc_save_offset = 40,
    .outgoing_arg_offset = 48,
    .incoming_arg_offset = 48,
    .gpr_arg_regs = ppc_gpr_arg_regs,
    .num_gpr_arg_regs = sizeof(ppc_gpr_arg_regs) / sizeof(ppc_gpr_arg_regs[0]),
    .fpr_arg_regs = ppc64_fpr_arg_regs,
    .num_fpr_arg_regs = sizeof(ppc64_fpr_arg_regs) / sizeof(ppc64_fpr_arg_regs[0]),
    .gpr_return_reg = 3,
    .fpr_return_reg = 1,
    .indirect_call_reg = 12,
    .alloc_gpr_regs = ppc_alloc_gpr_regs,
    .num_alloc_gpr_regs = sizeof(ppc_alloc_gpr_regs) / sizeof(ppc_alloc_gpr_regs[0]),
    .alloc_fpr_regs = ppc_alloc_fpr_regs,
    .num_alloc_fpr_regs = sizeof(ppc_alloc_fpr_regs) / sizeof(ppc_alloc_fpr_regs[0]),
    .scratch_gpr_regs = ppc_scratch_gpr_regs,
    .num_scratch_gpr_regs = sizeof(ppc_scratch_gpr_regs) / sizeof(ppc_scratch_gpr_regs[0]),
    .scratch_fpr_regs = ppc_scratch_fpr_regs,
    .num_scratch_fpr_regs = sizeof(ppc_scratch_fpr_regs) / sizeof(ppc_scratch_fpr_regs[0]),
    .abi_ops = &ppc_elfv1_abi_ops,
    .arch_info = &ppc64_arch_info
};

const anvil_backend_ops_t anvil_backend_ppc64 = {
    .name = "PowerPC 64-bit",
    .arch = ANVIL_ARCH_PPC64,
    ANVIL_PPC_BACKEND_OPS
};
