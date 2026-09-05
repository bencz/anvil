#include "machine_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
    size_t *successor_offsets;
    size_t *predecessor_offsets;
    size_t *successors;
    size_t *predecessors;
} liveness_edges_t;

static void destroy_edges(liveness_edges_t *edges)
{
    free(edges->successor_offsets);
    free(edges->predecessor_offsets);
    free(edges->successors);
    free(edges->predecessors);
    memset(edges, 0, sizeof(*edges));
}

static bool build_edges(const anvil_mir_func_t *func, liveness_edges_t *edges)
{
    size_t blocks = func->num_blocks;
    if (blocks == SIZE_MAX || blocks + 1 > SIZE_MAX / sizeof(size_t))
        return false;

    edges->successor_offsets = calloc(blocks + 1, sizeof(size_t));
    edges->predecessor_offsets = calloc(blocks + 1, sizeof(size_t));
    if (!edges->successor_offsets || !edges->predecessor_offsets)
        return false;

    size_t count = 0;
    for (size_t index = 0; index < func->num_instrs; index++)
    {
        const anvil_mir_instr_t *instr = &func->instrs[index];
        if (instr->block >= blocks)
            return false;

        const anvil_mir_block_t targets[] = { instr->true_block, instr->false_block };
        for (size_t edge = 0; edge < 2; edge++)
        {
            if (targets[edge] == ANVIL_MIR_NO_BLOCK)
                continue;

            if (targets[edge] >= blocks || count == SIZE_MAX / sizeof(size_t))
                return false;

            edges->successor_offsets[instr->block + 1]++;
            edges->predecessor_offsets[targets[edge] + 1]++;
            count++;
        }
    }

    for (size_t block = 0; block < blocks; block++)
    {
        edges->successor_offsets[block + 1] += edges->successor_offsets[block];
        edges->predecessor_offsets[block + 1] += edges->predecessor_offsets[block];
    }

    edges->successors = calloc(count ? count : 1, sizeof(size_t));
    edges->predecessors = calloc(count ? count : 1, sizeof(size_t));
    size_t *next_successor = calloc(blocks, sizeof(size_t));
    size_t *next_predecessor = calloc(blocks, sizeof(size_t));
    if (!edges->successors || !edges->predecessors || !next_successor || !next_predecessor)
    {
        free(next_successor);
        free(next_predecessor);
        return false;
    }

    for (size_t index = 0; index < func->num_instrs; index++)
    {
        const anvil_mir_instr_t *instr = &func->instrs[index];
        const anvil_mir_block_t targets[] = { instr->true_block, instr->false_block };
        for (size_t edge = 0; edge < 2; edge++)
        {
            size_t target = targets[edge];
            if (target == ANVIL_MIR_NO_BLOCK)
                continue;

            size_t successor = edges->successor_offsets[instr->block] + next_successor[instr->block]++;
            size_t predecessor = edges->predecessor_offsets[target] + next_predecessor[target]++;
            edges->successors[successor] = target;
            edges->predecessors[predecessor] = instr->block;
        }
    }

    free(next_successor);
    free(next_predecessor);
    return true;
}

void anvil_mir_liveness_destroy(anvil_mir_liveness_t *result)
{
    if (!result)
        return;

    free(result->live_in);
    free(result->live_out);
    free(result->first_instr);
    free(result->last_instr);
    memset(result, 0, sizeof(*result));
}

bool anvil_mir_compute_liveness(const anvil_mir_func_t *func, anvil_mir_liveness_t *result)
{
    if (!func || !result)
        return false;

    memset(result, 0, sizeof(*result));
    size_t blocks = func->num_blocks;
    size_t words = func->num_vregs / 64 + (func->num_vregs % 64 != 0);
    if (blocks == 0 || words == 0)
        return true;

    if (blocks > SIZE_MAX / words || blocks * words > SIZE_MAX / sizeof(uint64_t) || blocks > SIZE_MAX / sizeof(size_t))
        return false;

    size_t cells = blocks * words;
    uint64_t *use = calloc(cells, sizeof(*use));
    uint64_t *def = calloc(cells, sizeof(*def));
    bool *queued = calloc(blocks, sizeof(*queued));
    size_t *queue = calloc(blocks, sizeof(*queue));
    liveness_edges_t edges = { 0 };
    result->words_per_block = words;
    result->live_in = calloc(cells, sizeof(uint64_t));
    result->live_out = calloc(cells, sizeof(uint64_t));
    result->first_instr = calloc(blocks, sizeof(size_t));
    result->last_instr = calloc(blocks, sizeof(size_t));
    bool ok = false;
    if (!use || !def || !queued || !queue || !result->live_in || !result->live_out || !result->first_instr || !result->last_instr)
        goto cleanup;

    if (!build_edges(func, &edges))
        goto cleanup;

    for (size_t block = 0; block < blocks; block++)
    {
        result->first_instr[block] = SIZE_MAX;
        queue[block] = blocks - block - 1;
        queued[block] = true;
    }

    for (size_t index = 0; index < func->num_instrs; index++)
    {
        const anvil_mir_instr_t *instr = &func->instrs[index];
        size_t block = instr->block;
        size_t row = block * words;
        if (result->first_instr[block] == SIZE_MAX)
            result->first_instr[block] = index;

        result->last_instr[block] = index;
        for (size_t operand = 0; operand < instr->num_uses; operand++)
        {
            anvil_mir_vreg_t value = instr->uses[operand];
            if (!anvil_mir_valid_vreg(func, value))
                goto cleanup;

            uint64_t bit = UINT64_C(1) << (value % 64);
            size_t cell = row + value / 64;
            use[cell] |= bit & ~def[cell];
        }

        if (instr->def != ANVIL_MIR_NO_VREG)
        {
            if (!anvil_mir_valid_vreg(func, instr->def))
                goto cleanup;

            def[row + instr->def / 64] |= UINT64_C(1) << (instr->def % 64);
        }
    }

    size_t head = 0;
    size_t tail = 0;
    size_t pending = blocks;
    while (pending)
    {
        size_t block = queue[head];
        head = (head + 1) % blocks;
        pending--;
        queued[block] = false;
        bool changed = false;
        size_t row = block * words;

        for (size_t word = 0; word < words; word++)
        {
            uint64_t out = 0;
            for (size_t edge = edges.successor_offsets[block]; edge < edges.successor_offsets[block + 1]; edge++)
                out |= result->live_in[edges.successors[edge] * words + word];

            uint64_t in = use[row + word] | (out & ~def[row + word]);
            changed |= in != result->live_in[row + word];
            result->live_in[row + word] = in;
            result->live_out[row + word] = out;
        }

        if (!changed)
            continue;

        for (size_t edge = edges.predecessor_offsets[block]; edge < edges.predecessor_offsets[block + 1]; edge++)
        {
            size_t predecessor = edges.predecessors[edge];
            if (queued[predecessor])
                continue;

            queue[tail] = predecessor;
            tail = (tail + 1) % blocks;
            pending++;
            queued[predecessor] = true;
        }
    }

    ok = true;

cleanup:
    free(use);
    free(def);
    free(queued);
    free(queue);
    destroy_edges(&edges);
    if (!ok)
        anvil_mir_liveness_destroy(result);

    return ok;
}
