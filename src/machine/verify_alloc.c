#include "machine_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool anvil_mir_verify_clobbers(const anvil_mir_func_t *func, char *error, size_t error_len)
{
    if (!func->assignments || !func->num_vregs)
        return true;

    anvil_mir_liveness_t liveness;
    if (!anvil_mir_compute_liveness(func, &liveness))
    {
        if (error && error_len)
            snprintf(error, error_len, "out of memory verifying physical clobbers");

        return false;
    }

    uint64_t *live = calloc(liveness.words_per_block, sizeof(*live));
    size_t *previous = calloc(func->num_instrs, sizeof(*previous));
    size_t *last = calloc(func->num_blocks, sizeof(*last));
    bool valid = live && previous && last;
    if (!valid)
    {
        if (error && error_len)
            snprintf(error, error_len, "out of memory verifying physical clobbers");

        goto done;
    }

    for (size_t block = 0; block < func->num_blocks; block++)
        last[block] = SIZE_MAX;

    for (size_t index = 0; index < func->num_instrs; index++)
    {
        size_t block = func->instrs[index].block;
        previous[index] = last[block];
        last[block] = index;
    }

    for (size_t block = 0; block < func->num_blocks; block++)
    {
        memcpy(live, &liveness.live_out[block * liveness.words_per_block], liveness.words_per_block * sizeof(*live));
        for (size_t index = last[block]; index != SIZE_MAX; index = previous[index])
        {
            const anvil_mir_instr_t *instr = &func->instrs[index];
            if (instr->def != ANVIL_MIR_NO_VREG)
                live[instr->def / 64] &= ~(UINT64_C(1) << (instr->def % 64));

            bool has_clobbers = false;
            for (size_t reg_class = 0; reg_class < ANVIL_MIR_REG_CLASS_COUNT; reg_class++)
                has_clobbers |= instr->clobbers[reg_class] != 0;

            if (has_clobbers)
            {
                for (size_t value = 0; value < func->num_vregs; value++)
                {
                    const anvil_regalloc_assignment_t *assignment = &func->assignments[value];
                    if (!(live[value / 64] & (UINT64_C(1) << (value % 64))) || assignment->spilled ||
                        assignment->phys_reg < 0 || assignment->phys_reg >= 64)
                        continue;

                    anvil_mir_reg_class_t reg_class = func->vregs[value].reg_class;
                    if (instr->clobbers[reg_class] & (UINT64_C(1) << assignment->phys_reg))
                    {
                        if (error && error_len)
                            snprintf(error, error_len, "instruction %zu clobbers register %d holding live vreg %zu", index, assignment->phys_reg, value);

                        valid = false;
                        goto done;
                    }
                }
            }

            /* Inputs are consumed before implicit writes; results are defined
             * afterwards. Only values live across the instruction must survive. */
            for (size_t operand = 0; operand < instr->num_uses; operand++)
            {
                size_t value = instr->uses[operand];
                live[value / 64] |= UINT64_C(1) << (value % 64);
            }
        }
    }

done:
    free(live);
    free(previous);
    free(last);
    anvil_mir_liveness_destroy(&liveness);
    return valid;
}
