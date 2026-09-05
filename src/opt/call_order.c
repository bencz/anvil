#include "opt_utils.h"

#include <stdlib.h>

typedef struct {
    anvil_func_t *func;
    unsigned char state;
} call_node;

typedef struct {
    size_t node;
    anvil_block_t *block;
    anvil_instr_t *instruction;
} call_frame;

static int compare_nodes(const void *left, const void *right)
{
    const call_node *a = left;
    const call_node *b = right;
    return (a->func->id > b->func->id) - (a->func->id < b->func->id);
}

static size_t find_node(const call_node *nodes, size_t count, const anvil_func_t *func)
{
    size_t low = 0;
    size_t high = count;
    while (low < high)
    {
        size_t middle = low + (high - low) / 2;
        if (nodes[middle].func->id < func->id)
            low = middle + 1;
        else
            high = middle;
    }

    return low < count && nodes[low].func == func ? low : SIZE_MAX;
}

static anvil_func_t *next_callee(call_frame *frame)
{
    while (frame->block)
    {
        while (frame->instruction)
        {
            anvil_instr_t *instruction = frame->instruction;
            frame->instruction = instruction->next;
            if (instruction->op != ANVIL_OP_CALL || !instruction->num_operands)
                continue;

            anvil_value_t *value = instruction->operands[0];
            if (value->kind == ANVIL_VAL_CONST_SYMBOL_ADDR)
                value = value->data.reloc.symbol;

            if (value && value->kind == ANVIL_VAL_FUNC)
                return value->data.func;
        }

        frame->block = frame->block->next;
        frame->instruction = frame->block ? frame->block->first : NULL;
    }

    return NULL;
}

/* Build a callee-first order without using the host stack for deep call graphs.
 * Back edges stay within their recursive component; the inliner excludes
 * recursive bodies. No function or module list is changed by this analysis. */
anvil_func_t **anvil_opt_call_order(anvil_module_t *module)
{
    size_t count = module->num_funcs;
    anvil_ctx_t *ctx = module->ctx;
    call_node *nodes = anvil_ctx_calloc(ctx, count, sizeof(*nodes));
    call_frame *frames = anvil_ctx_calloc(ctx, count, sizeof(*frames));
    anvil_func_t **order = anvil_ctx_calloc(ctx, count, sizeof(*order));
    if (!nodes || !frames || !order)
    {
        free(nodes);
        free(frames);
        free(order);
        return NULL;
    }

    size_t index = 0;
    for (anvil_func_t *func = module->funcs; func; func = func->next)
        nodes[index++].func = func;

    qsort(nodes, count, sizeof(*nodes), compare_nodes);
    size_t written = 0;
    for (size_t root = 0; root < count; root++)
    {
        if (nodes[root].state)
            continue;

        size_t depth = 1;
        nodes[root].state = 1;
        frames[0] = (call_frame){ root, nodes[root].func->blocks, NULL };
        frames[0].instruction = frames[0].block ? frames[0].block->first : NULL;
        while (depth)
        {
            call_frame *frame = &frames[depth - 1];
            anvil_func_t *callee = next_callee(frame);
            if (!callee)
            {
                nodes[frame->node].state = 2;
                order[written++] = nodes[frame->node].func;
                depth--;
                continue;
            }

            size_t child = find_node(nodes, count, callee);
            if (child == SIZE_MAX || nodes[child].state)
                continue;

            nodes[child].state = 1;
            frames[depth] = (call_frame){ child, callee->blocks, callee->blocks ? callee->blocks->first : NULL };
            depth++;
        }
    }

    free(nodes);
    free(frames);
    return order;
}
