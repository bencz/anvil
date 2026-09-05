/*
 * ANVIL - IBM z/Architecture backend wrapper.
 *
 * The implementation lives in the SystemZ family MachineIR pipeline.
 * This target binds HLASM to ANVIL's MVS-oriented arena linkage. Native z/OS
 * interoperability and the Linux s390x ABI are not established by this binding.
 */

#include "../systemz_internal.h"

const anvil_mainframe_target_desc_t zarch_target_desc = {
    .variant = ANVIL_MAINFRAME_VARIANT_ZARCH,
    .arch = ANVIL_ARCH_ZARCH,
    .name = "IBM Z/ARCHITECTURE",
    .ptr_size = 8,
    .addr_bits = 64,
    .word_size = 8,
    .num_fpr = 16,
    .save_area_size = 144,
    .fp_temp_offset = 144,
    .local_area_offset = 160,
    .amode = "64",
    .rmode = "ANY",
    .fp_format = ANVIL_FP_HFP_IEEE,
    .big_endian = true,
    .has_64bit_gprs = true,
    .supports_ieee_fp = true,
    .supports_dfp = true,
    .has_relative_branches = true,
    .param_list_reg = 1,
    .return_addr_reg = 14,
    .return_gpr = 15,
    .return_fpr = 0,
    .base_reg = 12,
    .save_area_reg = 13,
    .alloc_gpr_regs = systemz_alloc_gprs,
    .num_alloc_gpr_regs = sizeof(systemz_alloc_gprs) / sizeof(systemz_alloc_gprs[0]),
    .alloc_fpr_regs = systemz_s390_fprs,
    .num_alloc_fpr_regs = sizeof(systemz_s390_fprs) / sizeof(systemz_s390_fprs[0]),
    .scratch_gpr_regs = systemz_scratch_gprs,
    .num_scratch_gpr_regs = sizeof(systemz_scratch_gprs) / sizeof(systemz_scratch_gprs[0]),
    .scratch_fpr_regs = systemz_s390_scratch_fprs,
    .num_scratch_fpr_regs = sizeof(systemz_s390_scratch_fprs) / sizeof(systemz_s390_scratch_fprs[0]),
    .abi_ops = &systemz_mvs_arena_64_abi_ops,
    .asm_ops = &systemz_hlasm_ops
};

static const anvil_arch_info_t zarch_arch_info = {
    .arch = ANVIL_ARCH_ZARCH,
    .name = "z/Architecture",
    .ptr_size = 8,
    .addr_bits = 64,
    .word_size = 8,
    .num_gpr = 16,
    .num_fpr = 16,
    .endian = ANVIL_ENDIAN_BIG,
    .stack_dir = ANVIL_STACK_UP,
    .fp_format = ANVIL_FP_HFP_IEEE,
    .abi = ANVIL_ABI_MVS,
    .has_condition_codes = true,
    .has_delay_slots = false
};

static anvil_error_t zarch_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    (void)be;
    (void)ctx;
    return ANVIL_OK;
}

static void zarch_cleanup(anvil_backend_t *be)
{
    (void)be;
}

static void zarch_reset(anvil_backend_t *be)
{
    (void)be;
}

static const anvil_arch_info_t *zarch_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &zarch_arch_info;
}

static anvil_error_t zarch_codegen_module(anvil_backend_t *be, anvil_module_t *mod, char **output, size_t *len)
{
    return anvil_mainframe_codegen_module(be, mod, ANVIL_MAINFRAME_VARIANT_ZARCH, output, len);
}

static anvil_error_t zarch_codegen_func(anvil_backend_t *be, anvil_func_t *func, char **output, size_t *len)
{
    return anvil_mainframe_codegen_func(be, func, ANVIL_MAINFRAME_VARIANT_ZARCH, output, len);
}

const anvil_backend_ops_t anvil_backend_zarch = {
    .name = "z/Architecture",
    .arch = ANVIL_ARCH_ZARCH,
    .init = zarch_init,
    .cleanup = zarch_cleanup,
    .reset = zarch_reset,
    .codegen_module = zarch_codegen_module,
    .codegen_func = zarch_codegen_func,
    .get_arch_info = zarch_get_arch_info
};
