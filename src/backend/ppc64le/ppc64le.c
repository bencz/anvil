/*
 * ANVIL - PowerPC 64-bit little-endian backend wrapper.
 *
 * The implementation lives in the shared PowerPC MachineIR backend. This file
 * only exposes the PPC64LE ELFv2 target identity and selects the PPC64LE
 * descriptor.
 */

#include "anvil/anvil_ppc_mir.h"

static const anvil_arch_info_t ppc64le_arch_info = {
    .arch = ANVIL_ARCH_PPC64LE,
    .name = "PowerPC 64-bit LE",
    .ptr_size = 8,
    .addr_bits = 64,
    .word_size = 8,
    .num_gpr = 32,
    .num_fpr = 32,
    .endian = ANVIL_ENDIAN_LITTLE,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false
};

static anvil_error_t ppc64le_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    (void)be;
    (void)ctx;
    return ANVIL_OK;
}

static void ppc64le_cleanup(anvil_backend_t *be)
{
    (void)be;
}

static void ppc64le_reset(anvil_backend_t *be)
{
    (void)be;
}

static const anvil_arch_info_t *ppc64le_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &ppc64le_arch_info;
}

static anvil_error_t ppc64le_codegen_module(anvil_backend_t *be,
                                            anvil_module_t *mod,
                                            char **output,
                                            size_t *len)
{
    return anvil_ppc_codegen_module(be, mod, ANVIL_PPC_VARIANT_PPC64LE,
                                    output, len);
}

static anvil_error_t ppc64le_codegen_func(anvil_backend_t *be,
                                          anvil_func_t *func,
                                          char **output,
                                          size_t *len)
{
    return anvil_ppc_codegen_func(be, func, ANVIL_PPC_VARIANT_PPC64LE,
                                  output, len);
}

const anvil_backend_ops_t anvil_backend_ppc64le = {
    .name = "PowerPC 64-bit LE",
    .arch = ANVIL_ARCH_PPC64LE,
    .init = ppc64le_init,
    .cleanup = ppc64le_cleanup,
    .reset = ppc64le_reset,
    .codegen_module = ppc64le_codegen_module,
    .codegen_func = ppc64le_codegen_func,
    .get_arch_info = ppc64le_get_arch_info
};
