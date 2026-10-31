/*
 * ANVIL - PowerPC 32-bit backend wrapper.
 *
 * The implementation lives in the shared PowerPC MachineIR backend. This file
 * only exposes the PPC32 target identity and selects the PPC32 descriptor.
 */

#include "../ppc_internal.h"

static const anvil_arch_info_t ppc32_arch_info = {
    .arch = ANVIL_ARCH_PPC32,
    .name = "PowerPC 32-bit",
    .ptr_size = 4,
    .addr_bits = 32,
    .word_size = 4,
    .num_gpr = 32,
    .num_fpr = 32,
    .endian = ANVIL_ENDIAN_BIG,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false,
};

const anvil_ppc_target_desc_t ppc32_target_desc = {
    .variant = ANVIL_PPC_VARIANT_PPC32,
    .arch = ANVIL_ARCH_PPC32,
    .name = "ppc32",
    .word_size = 4,
    .little_endian = false,
    .uses_function_descriptors = false,
    .min_frame_size = 32,
    .lr_save_offset = 4,
    .toc_save_offset = 0,
    .outgoing_arg_offset = 8,
    .incoming_arg_offset = 8,
    .gpr_arg_regs = ppc_gpr_arg_regs,
    .num_gpr_arg_regs = sizeof(ppc_gpr_arg_regs) / sizeof(ppc_gpr_arg_regs[0]),
    .fpr_arg_regs = ppc32_fpr_arg_regs,
    .num_fpr_arg_regs = sizeof(ppc32_fpr_arg_regs) / sizeof(ppc32_fpr_arg_regs[0]),
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
    .abi_ops = &ppc_elf32_abi_ops,
    .arch_info = &ppc32_arch_info
};

const anvil_backend_ops_t anvil_backend_ppc32 = {
    .name = "PowerPC 32-bit",
    .arch = ANVIL_ARCH_PPC32,
    ANVIL_PPC_BACKEND_OPS
};
