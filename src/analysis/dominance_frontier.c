#include "anvil/anvil_analysis.h"

#include <stdlib.h>
#include <string.h>

void anvil_dominance_frontier_destroy(anvil_dominance_frontier_t *frontier)
{
    if (!frontier)
        return;

    free(frontier->offsets);
    free(frontier->blocks);
    memset(frontier, 0, sizeof(*frontier));
}

static bool visit_frontiers(const anvil_opt_cfg_t *cfg, size_t *marks, size_t *counts, size_t *blocks)
{
    for (size_t index = 0; index < cfg->count; index++)
        marks[index] = SIZE_MAX;

    for (size_t rank = 0; rank < cfg->reachable_count; rank++)
    {
        size_t join = cfg->rpo[rank];
        for (size_t edge = cfg->predecessor_offsets[join]; edge < cfg->predecessor_offsets[join + 1]; edge++)
        {
            size_t runner = cfg->predecessors[edge];
            if (cfg->rpo_rank[runner] == SIZE_MAX)
                continue;

            while (runner != SIZE_MAX && (runner != cfg->idom[join] || runner == join))
            {
                if (marks[runner] == join)
                    break;

                marks[runner] = join;
                if (counts[runner] == SIZE_MAX)
                    return false;

                if (blocks)
                    blocks[counts[runner]] = join;

                counts[runner]++;
                size_t parent = cfg->idom[runner];
                if (parent == runner)
                    break;

                runner = parent;
            }
        }
    }

    return true;
}

bool anvil_dominance_frontier_build(const anvil_opt_cfg_t *cfg, anvil_dominance_frontier_t *frontier)
{
    if (!cfg || !frontier || !cfg->count || cfg->entry >= cfg->count)
        return false;

    memset(frontier, 0, sizeof(*frontier));
    anvil_ctx_t *ctx = cfg->blocks[cfg->entry]->parent->owner_ctx;
    size_t *marks = anvil_ctx_calloc(ctx, cfg->count, sizeof(*marks));
    size_t *counts = anvil_ctx_calloc(ctx, cfg->count, sizeof(*counts));
    frontier->offsets = anvil_ctx_calloc(ctx, cfg->count + 1, sizeof(*frontier->offsets));
    if (!marks || !counts || !frontier->offsets)
        goto fail;

    if (!visit_frontiers(cfg, marks, counts, NULL))
        goto fail;

    size_t total = 0;
    for (size_t block = 0; block < cfg->count; block++)
    {
        if (counts[block] > SIZE_MAX - total)
            goto fail;

        size_t next = total + counts[block];
        frontier->offsets[block] = total;
        counts[block] = total;
        total = next;
    }
    frontier->offsets[cfg->count] = total;
    frontier->blocks = anvil_ctx_calloc(ctx, total ? total : 1, sizeof(*frontier->blocks));
    if (!frontier->blocks || !visit_frontiers(cfg, marks, counts, frontier->blocks))
        goto fail;

    frontier->count = cfg->count;
    free(counts);
    free(marks);
    return true;

fail:
    free(counts);
    free(marks);
    anvil_dominance_frontier_destroy(frontier);
    return false;
}
