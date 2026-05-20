/*
 * ANVIL - IBM S/390 backend wrapper.
 *
 * The implementation lives in the shared IBM mainframe MachineIR backend.
 * This target emits HLASM for MVS linkage with 31-bit ESA/390 addressing.
 */

#include "anvil/anvil_mainframe_mir.h"

static const anvil_arch_info_t s390_arch_info = {
    .arch = ANVIL_ARCH_S390,
    .name = "S/390",
    .ptr_size = 4,
    .addr_bits = 31,
    .word_size = 4,
    .num_gpr = 16,
    .num_fpr = 16,
    .endian = ANVIL_ENDIAN_BIG,
    .stack_dir = ANVIL_STACK_UP,
    .fp_format = ANVIL_FP_HFP,
    .abi = ANVIL_ABI_MVS,
    .has_condition_codes = true,
    .has_delay_slots = false
};

static anvil_error_t s390_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    (void)be;
    (void)ctx;
    return ANVIL_OK;
}

static void s390_cleanup(anvil_backend_t *be)
{
    (void)be;
}

static void s390_reset(anvil_backend_t *be)
{
    (void)be;
}

static const anvil_arch_info_t *s390_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &s390_arch_info;
}

static anvil_error_t s390_codegen_module(anvil_backend_t *be,
                                         anvil_module_t *mod,
                                         char **output,
                                         size_t *len)
{
    return anvil_mainframe_codegen_module(be, mod, ANVIL_MAINFRAME_VARIANT_S390,
                                          output, len);
}

static anvil_error_t s390_codegen_func(anvil_backend_t *be,
                                       anvil_func_t *func,
                                       char **output,
                                       size_t *len)
{
    return anvil_mainframe_codegen_func(be, func, ANVIL_MAINFRAME_VARIANT_S390,
                                        output, len);
}

const anvil_backend_ops_t anvil_backend_s390 = {
    .name = "S/390",
    .arch = ANVIL_ARCH_S390,
    .init = s390_init,
    .cleanup = s390_cleanup,
    .reset = s390_reset,
    .codegen_module = s390_codegen_module,
    .codegen_func = s390_codegen_func,
    .get_arch_info = s390_get_arch_info
};
