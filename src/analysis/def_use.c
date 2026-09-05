#include "anvil/anvil_analysis.h"

#include <stdlib.h>
#include <string.h>

static size_t value_hash(const anvil_value_t *value)
{
    uintptr_t bits = (uintptr_t)value;
    bits ^= bits >> 17;
    bits *= UINT32_C(0xed5ad4bb);
    bits ^= bits >> 11;
    return (size_t)bits;
}

size_t anvil_def_use_definition(const anvil_def_use_t *graph, const anvil_value_t *value)
{
    if (!graph || !graph->map_capacity || !value)
        return SIZE_MAX;

    size_t slot = value_hash(value) & (graph->map_capacity - 1);
    while (graph->value_map[slot])
    {
        size_t index = graph->value_map[slot] - 1;
        if (graph->instructions[index]->result == value)
            return index;

        slot = (slot + 1) & (graph->map_capacity - 1);
    }

    return SIZE_MAX;
}

void anvil_def_use_destroy(anvil_def_use_t *graph)
{
    if (!graph)
        return;

    free(graph->instructions);
    free(graph->use_offsets);
    free(graph->users);
    free(graph->value_map);
    memset(graph, 0, sizeof(*graph));
}

bool anvil_def_use_build(anvil_func_t *func, anvil_def_use_t *graph)
{
    if (!func || !func->parent || !graph)
        return false;

    memset(graph, 0, sizeof(*graph));
    anvil_ctx_t *ctx = func->parent->ctx;
    size_t operands = 0;
    for (anvil_block_t *block = func->blocks; block; block = block->next)
    {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
        {
            if (graph->count == SIZE_MAX || instr->num_operands > SIZE_MAX - operands)
                goto overflow;

            graph->count++;
            operands += instr->num_operands;
        }
    }

    if (!graph->count)
        return true;
    if (graph->count >= SIZE_MAX / (2 * sizeof(size_t)))
        goto overflow;

    graph->map_capacity = 4;
    while (graph->map_capacity / 2 < graph->count)
    {
        if (graph->map_capacity > SIZE_MAX / (2 * sizeof(size_t)))
            goto overflow;

        graph->map_capacity *= 2;
    }

    graph->instructions = anvil_ctx_calloc(ctx, graph->count, sizeof(*graph->instructions));
    graph->use_offsets = anvil_ctx_calloc(ctx, graph->count + 1, sizeof(*graph->use_offsets));
    graph->users = anvil_ctx_calloc(ctx, operands ? operands : 1, sizeof(*graph->users));
    graph->value_map = anvil_ctx_calloc(ctx, graph->map_capacity, sizeof(*graph->value_map));
    if (!graph->instructions || !graph->use_offsets || !graph->users || !graph->value_map)
        goto fail;

    size_t index = 0;
    for (anvil_block_t *block = func->blocks; block; block = block->next)
    {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
        {
            graph->instructions[index] = instr;
            if (instr->result)
            {
                size_t slot = value_hash(instr->result) & (graph->map_capacity - 1);
                while (graph->value_map[slot])
                    slot = (slot + 1) & (graph->map_capacity - 1);

                graph->value_map[slot] = index + 1;
            }

            index++;
        }
    }

    for (size_t user = 0; user < graph->count; user++)
    {
        anvil_instr_t *instr = graph->instructions[user];
        for (size_t operand = 0; operand < instr->num_operands; operand++)
        {
            size_t definition = anvil_def_use_definition(graph, instr->operands[operand]);
            if (definition != SIZE_MAX)
                graph->use_offsets[definition + 1]++;
        }
    }

    size_t *cursor = anvil_ctx_calloc(ctx, graph->count, sizeof(*cursor));
    if (!cursor)
        goto fail;

    for (size_t definition = 0; definition < graph->count; definition++)
    {
        graph->use_offsets[definition + 1] += graph->use_offsets[definition];
        cursor[definition] = graph->use_offsets[definition];
    }

    for (size_t user = 0; user < graph->count; user++)
    {
        anvil_instr_t *instr = graph->instructions[user];
        for (size_t operand = 0; operand < instr->num_operands; operand++)
        {
            size_t definition = anvil_def_use_definition(graph, instr->operands[operand]);
            if (definition != SIZE_MAX)
                graph->users[cursor[definition]++] = user;
        }
    }

    free(cursor);
    return true;

overflow:
    anvil_set_error(ctx, ANVIL_ERR_NOMEM, "Def-use analysis size overflow");

fail:
    anvil_def_use_destroy(graph);
    return false;
}
