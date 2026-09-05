/*
 * ANVIL - IBM S/370-XA backend wrapper.
 *
 * The implementation lives in the SystemZ family MachineIR pipeline.
 * This target emits HLASM for MVS linkage with 31-bit addressing.
 */

#include "../systemz_internal.h"

const anvil_mainframe_target_desc_t s370_xa_target_desc = {
    .variant = ANVIL_MAINFRAME_VARIANT_S370_XA,
    .arch = ANVIL_ARCH_S370_XA,
    .name = "IBM S/370-XA",
    .ptr_size = 4,
    .addr_bits = 31,
    .word_size = 4,
    .num_fpr = 4,
    .save_area_size = 72,
    .fp_temp_offset = 72,
    .local_area_offset = 88,
    .amode = "31",
    .rmode = "ANY",
    .fp_format = ANVIL_FP_HFP,
    .big_endian = true,
    .has_64bit_gprs = false,
    .supports_ieee_fp = false,
    .supports_dfp = false,
    .has_relative_branches = false,
    .param_list_reg = 1,
    .return_addr_reg = 14,
    .return_gpr = 15,
    .return_fpr = 0,
    .base_reg = 12,
    .save_area_reg = 13,
    .alloc_gpr_regs = systemz_alloc_gprs,
    .num_alloc_gpr_regs = sizeof(systemz_alloc_gprs) / sizeof(systemz_alloc_gprs[0]),
    .alloc_fpr_regs = systemz_s370_fprs,
    .num_alloc_fpr_regs = sizeof(systemz_s370_fprs) / sizeof(systemz_s370_fprs[0]),
    .scratch_gpr_regs = systemz_scratch_gprs,
    .num_scratch_gpr_regs = sizeof(systemz_scratch_gprs) / sizeof(systemz_scratch_gprs[0]),
    .scratch_fpr_regs = systemz_s370_scratch_fprs,
    .num_scratch_fpr_regs = sizeof(systemz_s370_scratch_fprs) / sizeof(systemz_s370_scratch_fprs[0]),
    .abi_ops = &systemz_mvs_arena_31_abi_ops,
    .asm_ops = &systemz_hlasm_ops
};

static const anvil_arch_info_t s370_xa_arch_info = {
    .arch = ANVIL_ARCH_S370_XA,
    .name = "S/370-XA",
    .ptr_size = 4,
    .addr_bits = 31,
    .word_size = 4,
    .num_gpr = 16,
    .num_fpr = 4,
    .endian = ANVIL_ENDIAN_BIG,
    .stack_dir = ANVIL_STACK_UP,
    .fp_format = ANVIL_FP_HFP,
    .abi = ANVIL_ABI_MVS,
    .has_condition_codes = true,
    .has_delay_slots = false
};

static anvil_error_t s370_xa_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    (void)be;
    (void)ctx;
    return ANVIL_OK;
}

static void s370_xa_cleanup(anvil_backend_t *be)
{
    (void)be;
}

static void s370_xa_reset(anvil_backend_t *be)
{
    (void)be;
}

static const anvil_arch_info_t *s370_xa_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &s370_xa_arch_info;
}

static anvil_error_t s370_xa_codegen_module(anvil_backend_t *be, anvil_module_t *mod, char **output, size_t *len)
{
    return anvil_mainframe_codegen_module(be, mod, ANVIL_MAINFRAME_VARIANT_S370_XA, output, len);
}

static anvil_error_t s370_xa_codegen_func(anvil_backend_t *be, anvil_func_t *func, char **output, size_t *len)
{
    return anvil_mainframe_codegen_func(be, func, ANVIL_MAINFRAME_VARIANT_S370_XA, output, len);
}

const anvil_backend_ops_t anvil_backend_s370_xa = {
    .name = "S/370-XA",
    .arch = ANVIL_ARCH_S370_XA,
    .init = s370_xa_init,
    .cleanup = s370_xa_cleanup,
    .reset = s370_xa_reset,
    .codegen_module = s370_xa_codegen_module,
    .codegen_func = s370_xa_codegen_func,
    .get_arch_info = s370_xa_get_arch_info
};
