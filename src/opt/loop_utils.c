#include "loop_utils.h"

#include <stdlib.h>

static bool outside_loop(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop, const anvil_block_t *block)
{
    return !anvil_loop_contains(loops, loop, anvil_opt_cfg_index(cfg, block));
}

static void append_prepared(anvil_block_t *block, anvil_instr_t *instr)
{
    instr->parent = block;
    instr->owner_module = block->owner_module;
    instr->prev = block->last;
    if (block->last)
        block->last->next = instr;
    else
        block->first = instr;

    block->last = instr;
    if (instr->result)
        instr->result->owner_module = block->owner_module;
}

static void redirect_edge(anvil_instr_t *terminator, anvil_block_t *header, anvil_block_t *preheader)
{
    if (terminator->true_block == header)
        terminator->true_block = preheader;

    if (terminator->false_block == header)
        terminator->false_block = preheader;

    for (size_t index = 0; index < terminator->num_switch_cases; index++)
    {
        if (terminator->switch_blocks[index] == header)
            terminator->switch_blocks[index] = preheader;
    }
}

anvil_block_t *anvil_opt_create_preheader(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop)
{
    size_t header_index = loops->loops[loop].header;
    anvil_block_t *header = cfg->blocks[header_index];
    anvil_func_t *func = header->parent;
    anvil_ctx_t *ctx = func->owner_ctx;
    size_t outside_count = 0;
    for (size_t edge = cfg->predecessor_offsets[header_index]; edge < cfg->predecessor_offsets[header_index + 1]; edge++)
        outside_count += !anvil_loop_contains(loops, loop, cfg->predecessors[edge]);

    if (!outside_count || header == func->entry)
        return NULL;

    size_t phi_count = 0;
    for (anvil_instr_t *phi = header->first; phi && phi->op == ANVIL_OP_PHI; phi = phi->next)
        phi_count++;

    anvil_instr_t **prepared = NULL;
    if (outside_count > 1 && phi_count)
    {
        prepared = anvil_ctx_calloc(ctx, phi_count, sizeof(*prepared));
        if (!prepared)
            return NULL;

        size_t index = 0;
        for (anvil_instr_t *phi = header->first; phi && phi->op == ANVIL_OP_PHI; phi = phi->next)
        {
            anvil_instr_t *incoming = anvil_instr_create(ctx, ANVIL_OP_PHI, phi->result->type, "loop.entry");
            if (!incoming)
                goto fail;

            prepared[index++] = incoming;
            if (!anvil_instr_reserve_operands(incoming, outside_count))
                goto fail;

            incoming->phi_blocks = anvil_ctx_calloc(ctx, outside_count, sizeof(*incoming->phi_blocks));
            if (!incoming->phi_blocks)
                goto fail;

            incoming->phi_capacity = outside_count;
            for (size_t item = 0; item < phi->num_phi_incoming; item++)
            {
                if (!outside_loop(cfg, loops, loop, phi->phi_blocks[item]))
                    continue;

                size_t out = incoming->num_phi_incoming++;
                incoming->operands[out] = phi->operands[item];
                incoming->phi_blocks[out] = phi->phi_blocks[item];
            }
            incoming->num_operands = incoming->num_phi_incoming;
        }
    }

    anvil_instr_t *branch = anvil_instr_create(ctx, ANVIL_OP_BR, ctx->type_void, NULL);
    if (!branch)
        goto fail;

    branch->true_block = header;

    /* Last potentially failing operation. Everything below commits prepared
     * nodes and shrinks existing PHI arrays without reallocating them. */
    anvil_block_t *preheader = anvil_block_create(func, "loop.preheader");
    if (!preheader)
        goto fail;

    size_t index = 0;
    for (anvil_instr_t *phi = header->first; phi && phi->op == ANVIL_OP_PHI; phi = phi->next)
    {
        anvil_value_t *incoming = NULL;
        size_t retained = 0;
        for (size_t item = 0; item < phi->num_phi_incoming; item++)
        {
            if (outside_loop(cfg, loops, loop, phi->phi_blocks[item]))
            {
                incoming = phi->operands[item];
                continue;
            }

            phi->operands[retained] = phi->operands[item];
            phi->phi_blocks[retained++] = phi->phi_blocks[item];
        }

        if (prepared)
        {
            append_prepared(preheader, prepared[index]);
            incoming = prepared[index++]->result;
        }

        phi->operands[retained] = incoming;
        phi->phi_blocks[retained] = preheader;
        phi->num_phi_incoming = retained + 1;
        phi->num_operands = retained + 1;
    }
    append_prepared(preheader, branch);

    for (size_t edge = cfg->predecessor_offsets[header_index]; edge < cfg->predecessor_offsets[header_index + 1]; edge++)
    {
        size_t predecessor = cfg->predecessors[edge];
        if (!anvil_loop_contains(loops, loop, predecessor))
            redirect_edge(cfg->blocks[predecessor]->last, header, preheader);
    }

    anvil_func_invalidate_cfg(func);
    free(prepared);
    return preheader;

fail:
    /* Detached instructions remain owned by the context, like other failed
     * builder results; no live block or incoming edge has been modified. */
    free(prepared);
    return NULL;
}
