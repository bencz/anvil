/*
 * ANVIL - PowerPC 32-bit backend wrapper.
 *
 * The implementation lives in the shared PowerPC MachineIR backend. This file
 * only exposes the PPC32 target identity and selects the PPC32 descriptor.
 */

#include "../ppc_internal.h"

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
    .abi_ops = &ppc_elf32_abi_ops
};

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
    .has_delay_slots = false
};

static anvil_error_t ppc32_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    (void)be;
    (void)ctx;
    return ANVIL_OK;
}

static void ppc32_cleanup(anvil_backend_t *be)
{
    (void)be;
}

static void ppc32_reset(anvil_backend_t *be)
{
    (void)be;
}

static const anvil_arch_info_t *ppc32_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &ppc32_arch_info;
}

static anvil_error_t ppc32_codegen_module(anvil_backend_t *be, anvil_module_t *mod, char **output, size_t *len)
{
    return anvil_ppc_codegen_module(be, mod, ANVIL_PPC_VARIANT_PPC32, output, len);
}

static anvil_error_t ppc32_codegen_func(anvil_backend_t *be, anvil_func_t *func, char **output, size_t *len)
{
    return anvil_ppc_codegen_func(be, func, ANVIL_PPC_VARIANT_PPC32, output, len);
}

const anvil_backend_ops_t anvil_backend_ppc32 = {
    .name = "PowerPC 32-bit",
    .arch = ANVIL_ARCH_PPC32,
    .init = ppc32_init,
    .cleanup = ppc32_cleanup,
    .reset = ppc32_reset,
    .codegen_module = ppc32_codegen_module,
    .codegen_func = ppc32_codegen_func,
    .get_arch_info = ppc32_get_arch_info
};
