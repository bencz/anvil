#include "ppc_internal.h"

const int ppc_gpr_arg_regs[] = {3, 4, 5, 6, 7, 8, 9, 10};
const int ppc32_fpr_arg_regs[] = {1, 2, 3, 4, 5, 6, 7, 8};
const int ppc64_fpr_arg_regs[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
/* Volatile registers first, so values that do not cross a call need no
 * save; r0 (no base), r2 (TOC), r11/r12 (spill scratch), r13 (thread) and
 * r31 (frame) are never allocated. */
const int ppc_alloc_gpr_regs[] = {10, 9, 8, 7, 6, 5, 4, 3, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30};
const int ppc_alloc_fpr_regs[] = {12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
const int ppc_scratch_gpr_regs[] = {11, 12};
const int ppc_scratch_fpr_regs[] = {0, 13};
const int ppc_alloc_vector_regs[PPC_NUM_ALLOC_VECTOR_REGS] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};
const int ppc_scratch_vector_regs[PPC_NUM_SCRATCH_VECTOR_REGS] = {18, 19};

static const anvil_ppc_target_desc_t *const targets[] = {&ppc32_target_desc, &ppc64_target_desc, &ppc64le_target_desc};

const anvil_ppc_target_desc_t *anvil_ppc_get_target_desc(anvil_ppc_variant_t variant)
{
    if ((size_t)variant >= sizeof(targets) / sizeof(targets[0]))
        return NULL;

    return targets[variant];
}
