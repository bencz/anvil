#include "anvil/anvil_analysis.h"

#include <stdlib.h>
#include <string.h>

bool anvil_loop_contains(const anvil_loop_analysis_t *analysis, size_t loop, size_t block)
{
    if (!analysis || loop >= analysis->count || block >= analysis->block_count)
        return false;

    return (analysis->members[loop * analysis->words + block / 64] & (UINT64_C(1) << (block % 64))) != 0;
}

static void add_member(anvil_loop_analysis_t *analysis, size_t loop, size_t block)
{
    analysis->members[loop * analysis->words + block / 64] |= UINT64_C(1) << (block % 64);
    analysis->loops[loop].member_count++;
}

void anvil_loop_analysis_destroy(anvil_loop_analysis_t *analysis)
{
    if (!analysis)
        return;

    free(analysis->loops);
    free(analysis->members);
    memset(analysis, 0, sizeof(*analysis));
}

static bool has_latch(const anvil_opt_cfg_t *cfg, size_t header)
{
    for (size_t edge = cfg->predecessor_offsets[header]; edge < cfg->predecessor_offsets[header + 1]; edge++)
    {
        if (anvil_opt_cfg_dominates(cfg, header, cfg->predecessors[edge]))
            return true;
    }

    return false;
}

static void collect_members(const anvil_opt_cfg_t *cfg, anvil_loop_analysis_t *analysis, size_t loop, size_t *worklist)
{
    anvil_loop_info_t *info = &analysis->loops[loop];
    size_t count = 0;
    add_member(analysis, loop, info->header);
    for (size_t edge = cfg->predecessor_offsets[info->header]; edge < cfg->predecessor_offsets[info->header + 1]; edge++)
    {
        size_t latch = cfg->predecessors[edge];
        if (!anvil_opt_cfg_dominates(cfg, info->header, latch))
            continue;

        info->latch_count++;
        if (!anvil_loop_contains(analysis, loop, latch))
        {
            add_member(analysis, loop, latch);
            worklist[count++] = latch;
        }
    }

    while (count)
    {
        size_t block = worklist[--count];
        for (size_t edge = cfg->predecessor_offsets[block]; edge < cfg->predecessor_offsets[block + 1]; edge++)
        {
            size_t predecessor = cfg->predecessors[edge];
            if (cfg->rpo_rank[predecessor] != SIZE_MAX && !anvil_loop_contains(analysis, loop, predecessor))
            {
                add_member(analysis, loop, predecessor);
                worklist[count++] = predecessor;
            }
        }
    }

    size_t outside_count = 0;
    for (size_t edge = cfg->predecessor_offsets[info->header]; edge < cfg->predecessor_offsets[info->header + 1]; edge++)
    {
        size_t predecessor = cfg->predecessors[edge];
        if (!anvil_loop_contains(analysis, loop, predecessor))
        {
            outside_count++;
            info->preheader = predecessor;
        }
    }
    if (outside_count != 1 || cfg->successor_offsets[info->preheader + 1] - cfg->successor_offsets[info->preheader] != 1)
        info->preheader = SIZE_MAX;

    for (size_t block = 0; block < cfg->count; block++)
    {
        if (!anvil_loop_contains(analysis, loop, block))
            continue;

        for (size_t edge = cfg->successor_offsets[block]; edge < cfg->successor_offsets[block + 1]; edge++)
        {
            if (!anvil_loop_contains(analysis, loop, cfg->successors[edge]))
                info->exit_edge_count++;
        }
    }
}

bool anvil_loop_analysis_build(const anvil_opt_cfg_t *cfg, anvil_loop_analysis_t *analysis)
{
    if (!cfg || !analysis || !cfg->count || cfg->entry >= cfg->count || cfg->count > SIZE_MAX - 63)
        return false;

    memset(analysis, 0, sizeof(*analysis));
    anvil_ctx_t *ctx = cfg->blocks[cfg->entry]->parent->owner_ctx;
    for (size_t rank = 0; rank < cfg->reachable_count; rank++)
    {
        if (has_latch(cfg, cfg->rpo[rank]))
            analysis->count++;
    }

    analysis->block_count = cfg->count;
    analysis->words = (cfg->count + 63) / 64;
    if (!analysis->count)
        return true;

    if (analysis->count > SIZE_MAX / analysis->words)
        goto fail;

    analysis->loops = anvil_ctx_calloc(ctx, analysis->count, sizeof(*analysis->loops));
    analysis->members = anvil_ctx_calloc(ctx, analysis->count * analysis->words, sizeof(*analysis->members));
    size_t *worklist = anvil_ctx_calloc(ctx, cfg->count, sizeof(*worklist));
    if (!analysis->loops || !analysis->members || !worklist)
    {
        free(worklist);
        goto fail;
    }

    size_t loop = 0;
    for (size_t rank = 0; rank < cfg->reachable_count; rank++)
    {
        size_t header = cfg->rpo[rank];
        if (!has_latch(cfg, header))
            continue;

        analysis->loops[loop].header = header;
        analysis->loops[loop].preheader = SIZE_MAX;
        analysis->loops[loop].parent = SIZE_MAX;
        collect_members(cfg, analysis, loop++, worklist);
    }
    free(worklist);

    for (loop = 0; loop < analysis->count; loop++)
    {
        anvil_loop_info_t *info = &analysis->loops[loop];
        size_t parent_size = SIZE_MAX;
        for (size_t candidate = 0; candidate < analysis->count; candidate++)
        {
            const anvil_loop_info_t *parent = &analysis->loops[candidate];
            if (parent->member_count > info->member_count && parent->member_count < parent_size && anvil_loop_contains(analysis, candidate, info->header))
            {
                info->parent = candidate;
                parent_size = parent->member_count;
            }
        }
    }

    for (loop = 0; loop < analysis->count; loop++)
    {
        size_t parent = loop;
        while (parent != SIZE_MAX)
        {
            analysis->loops[loop].depth++;
            parent = analysis->loops[parent].parent;
        }
    }

    return true;

fail:
    anvil_loop_analysis_destroy(analysis);
    return false;
}
