/*
 * ANVIL - IBM z/Architecture backend wrapper.
 *
 * The implementation lives in the shared IBM mainframe MachineIR backend.
 * This target emits HLASM for z/OS MVS linkage. In ANVIL, zarch is the
 * current s390x/z target; Linux s390x is intentionally not modeled here yet.
 */

#include "anvil/anvil_mainframe_mir.h"

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

static anvil_error_t zarch_codegen_module(anvil_backend_t *be,
                                          anvil_module_t *mod,
                                          char **output,
                                          size_t *len)
{
    return anvil_mainframe_codegen_module(be, mod, ANVIL_MAINFRAME_VARIANT_ZARCH,
                                          output, len);
}

static anvil_error_t zarch_codegen_func(anvil_backend_t *be,
                                        anvil_func_t *func,
                                        char **output,
                                        size_t *len)
{
    return anvil_mainframe_codegen_func(be, func, ANVIL_MAINFRAME_VARIANT_ZARCH,
                                        output, len);
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
