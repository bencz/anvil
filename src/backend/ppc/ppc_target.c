#include "ppc_internal.h"

const int ppc_gpr_arg_regs[] = {3, 4, 5, 6, 7, 8, 9, 10};
const int ppc32_fpr_arg_regs[] = {1, 2, 3, 4, 5, 6, 7, 8};
const int ppc64_fpr_arg_regs[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
const int ppc_alloc_gpr_regs[] = {14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30};
const int ppc_alloc_fpr_regs[] = {14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
const int ppc_scratch_gpr_regs[] = {11, 12};
const int ppc_scratch_fpr_regs[] = {0, 13};

static const anvil_ppc_target_desc_t *const targets[] = {&ppc32_target_desc, &ppc64_target_desc, &ppc64le_target_desc};

const anvil_ppc_target_desc_t *anvil_ppc_get_target_desc(anvil_ppc_variant_t variant)
{
    if ((size_t)variant >= sizeof(targets) / sizeof(targets[0]))
        return NULL;

    return targets[variant];
}
