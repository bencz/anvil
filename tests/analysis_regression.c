#include <anvil/anvil_analysis.h>
#include <stdio.h>
#include <stdlib.h>

static bool verify_frontiers(const anvil_opt_cfg_t *cfg)
{
    anvil_dominance_frontier_t frontier;
    if (!anvil_dominance_frontier_build(cfg, &frontier))
        return false;

    bool valid = true;
    for (size_t dominator = 0; dominator < cfg->count; dominator++)
    {
        for (size_t join = 0; join < cfg->count; join++)
        {
            bool expected = false;
            if (cfg->rpo_rank[join] != SIZE_MAX && (dominator == join || !anvil_opt_cfg_dominates(cfg, dominator, join)))
            {
                for (size_t edge = cfg->predecessor_offsets[join]; edge < cfg->predecessor_offsets[join + 1]; edge++)
                    expected |= anvil_opt_cfg_dominates(cfg, dominator, cfg->predecessors[edge]);
            }

            size_t matches = 0;
            for (size_t position = frontier.offsets[dominator]; position < frontier.offsets[dominator + 1]; position++)
                matches += frontier.blocks[position] == join;

            if (matches != (size_t)expected)
            {
                fprintf(stderr, "frontier mismatch: dominator=%zu join=%zu expected=%u matches=%zu\n", dominator, join, expected, matches);
                valid = false;
            }
        }
    }

    anvil_dominance_frontier_destroy(&frontier);
    return valid;
}

static bool test_graph(unsigned seed, bool nested)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "analysis");
    anvil_type_t *boolean = anvil_type_i1(ctx);
    anvil_type_t *type = anvil_type_func(ctx, anvil_type_void(ctx), &boolean, 1, false);
    anvil_func_t *func = anvil_func_create(module, "graph", type, ANVIL_LINK_EXTERNAL);
    anvil_block_t *blocks[37];
    size_t count = nested ? 7 : 37;
    blocks[0] = anvil_func_get_entry(func);
    for (size_t index = 1; index < count; index++)
    {
        char name[32];
        snprintf(name, sizeof(name), "block%zu", index);
        blocks[index] = anvil_block_create(func, name);
    }

    for (size_t index = 0; index < count; index++)
    {
        anvil_set_insert_point(ctx, blocks[index]);
        if (nested)
        {
            const size_t left[] = { 1, 2, 3, 4, 3, 1, 6 };
            const size_t right[] = { 1, 6, 3, 5, 3, 1, 6 };
            if (index == 6)
                anvil_build_ret_void(ctx);
            else if (left[index] == right[index])
                anvil_build_br(ctx, blocks[left[index]]);
            else
                anvil_build_br_cond(ctx, anvil_func_get_param(func, 0), blocks[left[index]], blocks[right[index]]);
        }
        else
        {
            seed = seed * 1664525u + 1013904223u;
            size_t destination = seed % count;
            if (index + 1 == count && (seed & 1))
                anvil_build_ret_void(ctx);
            else
                anvil_build_br_cond(ctx, anvil_func_get_param(func, 0), blocks[(index + 1) % count], blocks[destination]);
        }
    }

    anvil_opt_cfg_t cfg;
    bool valid = anvil_opt_cfg_build(func, &cfg);
    if (valid)
    {
        valid = verify_frontiers(&cfg);
        anvil_loop_analysis_t loops;
        if (!anvil_loop_analysis_build(&cfg, &loops))
        {
            valid = false;
        }
        else
        {
            for (size_t loop = 0; loop < loops.count; loop++)
            {
                for (size_t block = 0; block < cfg.count; block++)
                {
                    if (anvil_loop_contains(&loops, loop, block) && !anvil_opt_cfg_dominates(&cfg, loops.loops[loop].header, block))
                        valid = false;
                }
            }
            if (nested)
            {
                valid &= loops.count == 2;
                if (loops.count == 2)
                {
                    valid &= loops.loops[0].header == 1 && loops.loops[0].preheader == 0 && loops.loops[0].member_count == 5;
                    valid &= loops.loops[0].depth == 1 && loops.loops[0].exit_edge_count == 1 && loops.loops[0].latch_count == 1;
                    valid &= loops.loops[1].header == 3 && loops.loops[1].preheader == 2 && loops.loops[1].member_count == 2;
                    valid &= loops.loops[1].parent == 0 && loops.loops[1].depth == 2 && loops.loops[1].exit_edge_count == 1;
                }
            }
            anvil_loop_analysis_destroy(&loops);
        }
        anvil_opt_cfg_destroy(&cfg);
    }

    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

int main(void)
{
    if (!test_graph(0, true))
        return 1;

    for (unsigned seed = 0; seed < 128; seed++)
    {
        if (!test_graph(seed, false))
        {
            fprintf(stderr, "analysis regression at seed %u\n", seed);
            return 1;
        }
    }

    return 0;
}
