/* Speculate only nontrapping integer operations into loop preheaders. */
#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"
#include "loop_utils.h"

#include <stdlib.h>
#include <string.h>

static bool safe_to_speculate(anvil_op_t op)
{
    switch (op)
    {
        case ANVIL_OP_ADD:
        case ANVIL_OP_SUB:
        case ANVIL_OP_MUL:
        case ANVIL_OP_AND:
        case ANVIL_OP_OR:
        case ANVIL_OP_XOR:
        case ANVIL_OP_CMP_EQ:
        case ANVIL_OP_CMP_NE:
        case ANVIL_OP_CMP_LT:
        case ANVIL_OP_CMP_LE:
        case ANVIL_OP_CMP_GT:
        case ANVIL_OP_CMP_GE:
        case ANVIL_OP_CMP_ULT:
        case ANVIL_OP_CMP_ULE:
        case ANVIL_OP_CMP_UGT:
        case ANVIL_OP_CMP_UGE:
        case ANVIL_OP_TRUNC:
        case ANVIL_OP_ZEXT:
        case ANVIL_OP_SEXT:
            return true;
        default:
            return false;
    }
}

static bool operands_available(const anvil_opt_cfg_t *cfg, const anvil_instr_t *instr, size_t preheader)
{
    for (size_t operand = 0; operand < instr->num_operands; operand++)
    {
        const anvil_value_t *value = instr->operands[operand];
        if (value->kind != ANVIL_VAL_INSTR)
            continue;

        size_t definition = anvil_opt_cfg_index(cfg, value->data.instr->parent);
        if (!anvil_opt_cfg_dominates(cfg, definition, preheader))
            return false;
    }

    return true;
}

static void move_before_terminator(anvil_instr_t *instr, anvil_block_t *preheader)
{
    anvil_opt_erase_instr(instr);
    anvil_instr_t *terminator = preheader->last;
    instr->parent = preheader;
    instr->prev = terminator->prev;
    instr->next = terminator;
    if (terminator->prev)
        terminator->prev->next = instr;
    else
        preheader->first = instr;

    terminator->prev = instr;
}

static bool has_hoistable_root(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop)
{
    size_t header = loops->loops[loop].header;
    if (header == cfg->entry)
        return false;

    for (size_t block = 0; block < cfg->count; block++)
    {
        if (!anvil_loop_contains(loops, loop, block))
            continue;

        for (anvil_instr_t *instr = cfg->blocks[block]->first; instr; instr = instr->next)
        {
            if (!instr->result || !safe_to_speculate(instr->op))
                continue;

            bool available = true;
            for (size_t operand = 0; operand < instr->num_operands; operand++)
            {
                const anvil_value_t *value = instr->operands[operand];
                if (value->kind != ANVIL_VAL_INSTR)
                    continue;

                size_t definition = anvil_opt_cfg_index(cfg, value->data.instr->parent);
                if (anvil_loop_contains(loops, loop, definition) || !anvil_opt_cfg_dominates(cfg, definition, header))
                {
                    available = false;
                    break;
                }
            }

            if (available)
                return true;
        }
    }

    return false;
}

anvil_pass_result_t anvil_pass_licm(anvil_func_t *func)
{
    if (!func || !func->parent)
        return ANVIL_PASS_RUN_ERROR;

    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
    if (func->is_declaration || !func->blocks)
        return ANVIL_PASS_RUN_UNCHANGED;

    bool changed = false;
    anvil_opt_cfg_t cfg;
    anvil_loop_analysis_t loops;

rebuild:
    if (!anvil_opt_cfg_build(func, &cfg))
        return ANVIL_PASS_RUN_ERROR;

    if (!anvil_loop_analysis_build(&cfg, &loops))
    {
        anvil_opt_cfg_destroy(&cfg);
        return ANVIL_PASS_RUN_ERROR;
    }

    for (size_t loop = 0; loop < loops.count; loop++)
    {
        size_t preheader = loops.loops[loop].preheader;
        if (preheader == SIZE_MAX)
        {
            /* Avoid creating empty preheaders that simplify-cfg would remove
             * again on every fixed-point iteration. */
            if (!has_hoistable_root(&cfg, &loops, loop))
                continue;

            anvil_block_t *created = anvil_opt_create_preheader(&cfg, &loops, loop);
            anvil_loop_analysis_destroy(&loops);
            anvil_opt_cfg_destroy(&cfg);
            if (!created)
                return ANVIL_PASS_RUN_ERROR;

            changed = true;
            goto rebuild;
        }

        bool moved;
        do
        {
            moved = false;
            for (size_t index = 0; index < cfg.reachable_count; index++)
            {
                size_t block = cfg.rpo[index];
                if (!anvil_loop_contains(&loops, loop, block))
                    continue;

                anvil_instr_t *instr = cfg.blocks[block]->first;
                while (instr)
                {
                    anvil_instr_t *next = instr->next;
                    if (instr->result && safe_to_speculate(instr->op) && operands_available(&cfg, instr, preheader))
                    {
                        move_before_terminator(instr, cfg.blocks[preheader]);
                        moved = true;
                        changed = true;
                    }

                    instr = next;
                }
            }
        } while (moved);
    }

    anvil_loop_analysis_destroy(&loops);
    anvil_opt_cfg_destroy(&cfg);
    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}
