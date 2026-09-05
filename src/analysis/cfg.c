#include "anvil/anvil_analysis.h"

#include <stdlib.h>
#include <string.h>

static size_t block_hash(const anvil_block_t *block)
{
    uintptr_t value = (uintptr_t)block;
    value ^= value >> 17;
    value *= UINT32_C(0xed5ad4bb);
    value ^= value >> 11;
    return (size_t)value;
}

size_t anvil_opt_cfg_index(const anvil_opt_cfg_t *cfg, const anvil_block_t *block)
{
    if (!cfg || !block || !cfg->map_capacity)
        return SIZE_MAX;

    size_t slot = block_hash(block) & (cfg->map_capacity - 1);
    while (cfg->block_map[slot])
    {
        size_t index = cfg->block_map[slot] - 1;
        if (cfg->blocks[index] == block)
            return index;

        slot = (slot + 1) & (cfg->map_capacity - 1);
    }

    return SIZE_MAX;
}

void anvil_opt_cfg_destroy(anvil_opt_cfg_t *cfg)
{
    if (!cfg)
        return;

    if (cfg->references && --*cfg->references)
    {
        memset(cfg, 0, sizeof(*cfg));
        return;
    }

    free(cfg->blocks);
    free(cfg->successor_offsets);
    free(cfg->successors);
    free(cfg->predecessor_offsets);
    free(cfg->predecessors);
    free(cfg->rpo);
    free(cfg->rpo_rank);
    free(cfg->idom);
    free(cfg->block_map);
    free(cfg->source_offsets);
    free(cfg->source_targets);
    free(cfg->references);
    memset(cfg, 0, sizeof(*cfg));
}

static size_t successor_count(const anvil_instr_t *terminator)
{
    if (terminator->op == ANVIL_OP_BR)
        return 1;
    if (terminator->op == ANVIL_OP_BR_COND)
        return 2;
    if (terminator->op == ANVIL_OP_SWITCH)
        return 1 + terminator->num_switch_cases;

    return 0;
}

static anvil_block_t *successor_at(const anvil_instr_t *terminator, size_t index)
{
    if (!index)
        return terminator->true_block;
    if (terminator->op == ANVIL_OP_BR_COND)
        return terminator->false_block;

    return terminator->switch_blocks[index - 1];
}

void anvil_func_invalidate_cfg(anvil_func_t *func)
{
    if (!func || !func->cfg_cache)
        return;

    anvil_opt_cfg_destroy(func->cfg_cache);
    free(func->cfg_cache);
    func->cfg_cache = NULL;
}

/* Compare exact edges in linear time instead of trusting a hash or requiring
 * every existing/custom pass to remember a mutation notification. */
static bool cache_matches(const anvil_func_t *func, const anvil_opt_cfg_t *cfg)
{
    if (!cfg || !cfg->count || func->entry != cfg->blocks[cfg->entry])
        return false;

    size_t index = 0;
    for (anvil_block_t *block = func->blocks; block; block = block->next)
    {
        if (index >= cfg->count || block != cfg->blocks[index] || !block->last || block->last->num_switch_cases == SIZE_MAX)
            return false;

        size_t count = successor_count(block->last);
        size_t first = cfg->source_offsets[index];
        if (count != cfg->source_offsets[index + 1] - first)
            return false;

        for (size_t edge = 0; edge < count; edge++)
        {
            if (successor_at(block->last, edge) != cfg->blocks[cfg->source_targets[first + edge]])
                return false;
        }

        index++;
    }

    return index == cfg->count;
}

/* Iterative DFS avoids exhausting the host stack on generated CFGs. */
static void compute_rpo(anvil_opt_cfg_t *cfg, size_t *stack, size_t *cursor)
{
    size_t depth = 1;
    stack[0] = cfg->entry;
    cursor[0] = cfg->successor_offsets[cfg->entry];
    cfg->rpo_rank[cfg->entry] = 0;

    while (depth)
    {
        size_t block = stack[depth - 1];
        size_t edge = cursor[depth - 1];
        if (edge < cfg->successor_offsets[block + 1])
        {
            cursor[depth - 1]++;
            size_t successor = cfg->successors[edge];
            if (cfg->rpo_rank[successor] == SIZE_MAX)
            {
                cfg->rpo_rank[successor] = 0;
                stack[depth] = successor;
                cursor[depth] = cfg->successor_offsets[successor];
                depth++;
            }
        }
        else
        {
            cfg->rpo[cfg->reachable_count++] = block;
            depth--;
        }
    }

    for (size_t index = 0; index < cfg->reachable_count / 2; index++)
    {
        size_t opposite = cfg->reachable_count - 1 - index;
        size_t block = cfg->rpo[index];
        cfg->rpo[index] = cfg->rpo[opposite];
        cfg->rpo[opposite] = block;
    }

    for (size_t index = 0; index < cfg->reachable_count; index++)
        cfg->rpo_rank[cfg->rpo[index]] = index;
}

static size_t intersect_dominators(const anvil_opt_cfg_t *cfg, size_t left, size_t right)
{
    while (left != right)
    {
        if (cfg->rpo_rank[left] > cfg->rpo_rank[right])
            left = cfg->idom[left];
        else
            right = cfg->idom[right];
    }

    return left;
}

static void compute_dominators(anvil_opt_cfg_t *cfg)
{
    cfg->idom[cfg->entry] = cfg->entry;
    bool changed;
    do
    {
        changed = false;
        for (size_t rank = 1; rank < cfg->reachable_count; rank++)
        {
            size_t block = cfg->rpo[rank];
            size_t parent = SIZE_MAX;
            for (size_t edge = cfg->predecessor_offsets[block]; edge < cfg->predecessor_offsets[block + 1]; edge++)
            {
                size_t predecessor = cfg->predecessors[edge];
                if (cfg->idom[predecessor] == SIZE_MAX)
                    continue;

                parent = parent == SIZE_MAX ? predecessor : intersect_dominators(cfg, parent, predecessor);
            }

            if (cfg->idom[block] != parent)
            {
                cfg->idom[block] = parent;
                changed = true;
            }
        }
    } while (changed);
}

bool anvil_opt_cfg_dominates(const anvil_opt_cfg_t *cfg, size_t dominator, size_t block)
{
    if (!cfg || dominator >= cfg->count || block >= cfg->count || cfg->idom[block] == SIZE_MAX)
        return false;

    while (block != dominator && block != cfg->idom[block])
        block = cfg->idom[block];

    return block == dominator;
}

bool anvil_opt_cfg_build(anvil_func_t *func, anvil_opt_cfg_t *cfg)
{
    if (!func || !func->parent || !cfg)
        return false;

    memset(cfg, 0, sizeof(*cfg));
    anvil_ctx_t *ctx = func->parent->ctx;
    if (cache_matches(func, func->cfg_cache))
    {
        if (*func->cfg_cache->references == SIZE_MAX)
            goto overflow;

        *cfg = *func->cfg_cache;
        (*cfg->references)++;
        return true;
    }

    anvil_func_invalidate_cfg(func);
    size_t edges = 0;
    for (anvil_block_t *block = func->blocks; block; block = block->next)
    {
        if (!block->last || cfg->count == SIZE_MAX || block->last->num_switch_cases == SIZE_MAX)
            goto invalid;

        size_t count = successor_count(block->last);
        if (count > SIZE_MAX - edges)
            goto overflow;

        edges += count;
        cfg->count++;
    }

    if (!cfg->count)
        goto invalid;
    if (cfg->count >= SIZE_MAX / (2 * sizeof(size_t)) || edges > SIZE_MAX / sizeof(size_t))
        goto overflow;

    cfg->map_capacity = 4;
    while (cfg->map_capacity / 2 < cfg->count)
    {
        if (cfg->map_capacity > SIZE_MAX / (2 * sizeof(size_t)))
            goto overflow;

        cfg->map_capacity *= 2;
    }

    cfg->blocks = anvil_ctx_calloc(ctx, cfg->count, sizeof(*cfg->blocks));
    cfg->block_map = anvil_ctx_calloc(ctx, cfg->map_capacity, sizeof(*cfg->block_map));
    cfg->successor_offsets = anvil_ctx_calloc(ctx, cfg->count + 1, sizeof(size_t));
    cfg->predecessor_offsets = anvil_ctx_calloc(ctx, cfg->count + 1, sizeof(size_t));
    cfg->successors = anvil_ctx_calloc(ctx, edges ? edges : 1, sizeof(size_t));
    cfg->predecessors = anvil_ctx_calloc(ctx, edges ? edges : 1, sizeof(size_t));
    cfg->rpo = anvil_ctx_calloc(ctx, cfg->count, sizeof(size_t));
    cfg->rpo_rank = anvil_ctx_calloc(ctx, cfg->count, sizeof(size_t));
    cfg->idom = anvil_ctx_calloc(ctx, cfg->count, sizeof(size_t));
    cfg->source_offsets = anvil_ctx_calloc(ctx, cfg->count + 1, sizeof(size_t));
    cfg->source_targets = anvil_ctx_calloc(ctx, edges ? edges : 1, sizeof(size_t));
    if (!cfg->blocks || !cfg->block_map || !cfg->successor_offsets || !cfg->predecessor_offsets ||
        !cfg->successors || !cfg->predecessors || !cfg->rpo || !cfg->rpo_rank || !cfg->idom || !cfg->source_offsets || !cfg->source_targets)
        goto fail;

    size_t index = 0;
    for (anvil_block_t *block = func->blocks; block; block = block->next)
    {
        cfg->blocks[index] = block;
        cfg->rpo_rank[index] = SIZE_MAX;
        cfg->idom[index] = SIZE_MAX;
        size_t slot = block_hash(block) & (cfg->map_capacity - 1);
        while (cfg->block_map[slot])
            slot = (slot + 1) & (cfg->map_capacity - 1);

        cfg->block_map[slot] = ++index;
    }

    cfg->entry = anvil_opt_cfg_index(cfg, func->entry);
    if (cfg->entry == SIZE_MAX)
        goto invalid;

    /* Deduplicate parallel switch/conditional edges: PHIs have one value per predecessor. */
    for (size_t block = 0; block < cfg->count; block++)
    {
        const anvil_instr_t *terminator = cfg->blocks[block]->last;
        size_t end = cfg->successor_offsets[block];
        size_t source = cfg->source_offsets[block];
        for (size_t edge = 0; edge < successor_count(terminator); edge++)
        {
            size_t target = anvil_opt_cfg_index(cfg, successor_at(terminator, edge));
            if (target == SIZE_MAX)
                goto invalid;

            cfg->source_targets[source++] = target;
            if (cfg->rpo_rank[target] == block)
                continue;

            cfg->rpo_rank[target] = block;
            cfg->successors[end++] = target;
            cfg->predecessor_offsets[target + 1]++;
        }

        cfg->successor_offsets[block + 1] = end;
        cfg->source_offsets[block + 1] = source;
    }

    for (size_t block = 0; block < cfg->count; block++)
    {
        cfg->predecessor_offsets[block + 1] += cfg->predecessor_offsets[block];
        cfg->rpo_rank[block] = SIZE_MAX;
        cfg->rpo[block] = cfg->predecessor_offsets[block];
    }

    for (size_t block = 0; block < cfg->count; block++)
    {
        for (size_t edge = cfg->successor_offsets[block]; edge < cfg->successor_offsets[block + 1]; edge++)
        {
            size_t target = cfg->successors[edge];
            cfg->predecessors[cfg->rpo[target]++] = block;
        }
    }

    size_t *scratch = anvil_ctx_calloc(ctx, cfg->count * 2, sizeof(size_t));
    if (!scratch)
        goto fail;

    compute_rpo(cfg, scratch, scratch + cfg->count);
    compute_dominators(cfg);
    size_t tail = cfg->reachable_count;
    for (size_t block = 0; block < cfg->count; block++)
    {
        if (cfg->rpo_rank[block] == SIZE_MAX)
            cfg->rpo[tail++] = block;
    }

    free(scratch);
    cfg->references = anvil_ctx_calloc(ctx, 1, sizeof(*cfg->references));
    if (!cfg->references)
        goto fail;

    *cfg->references = 1;
    func->cfg_cache = anvil_ctx_calloc(ctx, 1, sizeof(*func->cfg_cache));
    if (!func->cfg_cache)
        goto fail;

    *cfg->references = 2;
    *func->cfg_cache = *cfg;
    return true;

invalid:
    anvil_set_error(ctx, ANVIL_ERR_INVALID_OP, "Cannot analyze malformed function CFG");
    goto fail;

overflow:
    anvil_set_error(ctx, ANVIL_ERR_NOMEM, "CFG analysis size overflow");

fail:
    anvil_opt_cfg_destroy(cfg);
    return false;
}
