/*
 * ANVIL - PowerPC 32-bit backend wrapper.
 *
 * The implementation lives in the shared PowerPC MachineIR backend. This file
 * only exposes the PPC32 target identity and selects the PPC32 descriptor.
 */

#include "anvil/anvil_ppc_mir.h"

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

static anvil_error_t ppc32_codegen_module(anvil_backend_t *be,
                                          anvil_module_t *mod,
                                          char **output,
                                          size_t *len)
{
    return anvil_ppc_codegen_module(be, mod, ANVIL_PPC_VARIANT_PPC32,
                                    output, len);
}

static anvil_error_t ppc32_codegen_func(anvil_backend_t *be,
                                        anvil_func_t *func,
                                        char **output,
                                        size_t *len)
{
    return anvil_ppc_codegen_func(be, func, ANVIL_PPC_VARIANT_PPC32,
                                  output, len);
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
