/*
 * ANVIL - PowerPC 64-bit big-endian backend wrapper.
 *
 * The implementation lives in the shared PowerPC MachineIR backend. This file
 * only exposes the PPC64 ELFv1 target identity and selects the PPC64 descriptor.
 */

#include "anvil/anvil_ppc_mir.h"

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
    .has_delay_slots = false
};

static anvil_error_t ppc64_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    (void)be;
    (void)ctx;
    return ANVIL_OK;
}

static void ppc64_cleanup(anvil_backend_t *be)
{
    (void)be;
}

static void ppc64_reset(anvil_backend_t *be)
{
    (void)be;
}

static const anvil_arch_info_t *ppc64_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &ppc64_arch_info;
}

static anvil_error_t ppc64_codegen_module(anvil_backend_t *be,
                                          anvil_module_t *mod,
                                          char **output,
                                          size_t *len)
{
    return anvil_ppc_codegen_module(be, mod, ANVIL_PPC_VARIANT_PPC64,
                                    output, len);
}

static anvil_error_t ppc64_codegen_func(anvil_backend_t *be,
                                        anvil_func_t *func,
                                        char **output,
                                        size_t *len)
{
    return anvil_ppc_codegen_func(be, func, ANVIL_PPC_VARIANT_PPC64,
                                  output, len);
}

const anvil_backend_ops_t anvil_backend_ppc64 = {
    .name = "PowerPC 64-bit",
    .arch = ANVIL_ARCH_PPC64,
    .init = ppc64_init,
    .cleanup = ppc64_cleanup,
    .reset = ppc64_reset,
    .codegen_module = ppc64_codegen_module,
    .codegen_func = ppc64_codegen_func,
    .get_arch_info = ppc64_get_arch_info
};
