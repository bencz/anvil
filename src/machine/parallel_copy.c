#include "machine_internal.h"

#include <stdlib.h>

typedef struct {
    anvil_mir_parallel_copy_t copy;
    size_t remaining_users;
    size_t source_node;
    size_t first_user;
    size_t next_user;
    bool active;
} copy_node_t;

static size_t destination_slot(const copy_node_t *nodes, const size_t *map, size_t capacity, anvil_mir_vreg_t destination)
{
    size_t slot = ((size_t)destination * UINT32_C(0x9e3779b1)) & (capacity - 1);
    while (map[slot] && nodes[map[slot] - 1].copy.dst != destination)
        slot = (slot + 1) & (capacity - 1);

    return slot;
}

static bool emit_copy(anvil_mir_func_t *func, anvil_mir_vreg_t destination, anvil_mir_vreg_t source)
{
    return anvil_mir_add_instr(func, ANVIL_MIR_OP_COPY, destination, &source, 1);
}

static void rollback_copies(anvil_mir_func_t *func, size_t instructions, size_t vregs, anvil_mir_block_data_t block)
{
    for (size_t index = instructions; index < func->num_instrs; index++)
    {
        free(func->instrs[index].uses);
        free(func->instrs[index].symbol);
    }

    func->num_instrs = instructions;
    func->num_vregs = vregs;
    func->blocks[func->current_block] = block;
}

bool anvil_mir_emit_parallel_copies(anvil_mir_func_t *func, const anvil_mir_parallel_copy_t *copies, size_t count)
{
    if (!func || (count && !copies) || func->current_block >= func->num_blocks || func->assignments)
        return false;
    if (!count)
        return true;
    if (count > SIZE_MAX / sizeof(copy_node_t))
        return false;

    size_t capacity = 4;
    while (capacity / 2 < count)
    {
        if (capacity > SIZE_MAX / (2 * sizeof(size_t)))
            return false;

        capacity *= 2;
    }

    copy_node_t *nodes = calloc(count, sizeof(*nodes));
    size_t *map = calloc(capacity, sizeof(*map));
    size_t *ready = calloc(count, sizeof(*ready));
    if (!nodes || !map || !ready)
    {
        free(nodes);
        free(map);
        free(ready);
        return false;
    }

    bool valid = true;
    size_t pending = 0;
    for (size_t index = 0; index < count; index++)
    {
        const anvil_mir_vreg_info_t *destination = anvil_mir_get_vreg_info(func, copies[index].dst);
        const anvil_mir_vreg_info_t *source = anvil_mir_get_vreg_info(func, copies[index].src);
        if (!destination || !source || destination->reg_class != source->reg_class || destination->size_bits != source->size_bits)
        {
            valid = false;
            break;
        }

        size_t slot = destination_slot(nodes, map, capacity, copies[index].dst);
        if (map[slot])
        {
            valid = false;
            break;
        }

        nodes[index].copy = copies[index];
        nodes[index].source_node = SIZE_MAX;
        nodes[index].first_user = SIZE_MAX;
        nodes[index].next_user = SIZE_MAX;
        nodes[index].active = copies[index].dst != copies[index].src;
        map[slot] = index + 1;
        pending += nodes[index].active;
    }

    for (size_t index = 0; valid && index < count; index++)
    {
        if (!nodes[index].active)
            continue;

        size_t slot = destination_slot(nodes, map, capacity, nodes[index].copy.src);
        if (!map[slot] || !nodes[map[slot] - 1].active)
            continue;

        size_t source = map[slot] - 1;
        nodes[index].source_node = source;
        nodes[index].next_user = nodes[source].first_user;
        nodes[source].first_user = index;
        nodes[source].remaining_users++;
    }

    size_t head = 0;
    size_t tail = 0;
    for (size_t index = 0; valid && index < count; index++)
    {
        if (nodes[index].active && !nodes[index].remaining_users)
            ready[tail++] = index;
    }

    size_t original_instructions = func->num_instrs;
    size_t original_vregs = func->num_vregs;
    anvil_mir_block_data_t original_block = func->blocks[func->current_block];
    size_t cycle_cursor = 0;
    while (valid && pending)
    {
        if (head == tail)
        {
            while (cycle_cursor < count && !nodes[cycle_cursor].active)
                cycle_cursor++;

            copy_node_t *cycle = &nodes[cycle_cursor];
            const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(func, cycle->copy.dst);
            anvil_mir_vreg_t temporary = anvil_mir_add_vreg_typed(func, info->reg_class, info->size_bits, info->is_signed);
            if (temporary == ANVIL_MIR_NO_VREG || !emit_copy(func, temporary, cycle->copy.dst))
            {
                valid = false;
                break;
            }

            /* Save the original destination once, then redirect every remaining
             * reader. Adjacency lists keep cycle handling linear in copy count. */
            for (size_t user = cycle->first_user; user != SIZE_MAX; user = nodes[user].next_user)
            {
                if (nodes[user].active)
                {
                    nodes[user].copy.src = temporary;
                    nodes[user].source_node = SIZE_MAX;
                }
            }

            cycle->remaining_users = 0;
            ready[tail++] = cycle_cursor;
        }

        size_t index = ready[head++];
        copy_node_t *node = &nodes[index];
        if (!emit_copy(func, node->copy.dst, node->copy.src))
        {
            valid = false;
            break;
        }

        node->active = false;
        pending--;
        if (node->source_node != SIZE_MAX)
        {
            copy_node_t *source = &nodes[node->source_node];
            if (--source->remaining_users == 0)
                ready[tail++] = node->source_node;
        }
    }

    if (!valid)
        rollback_copies(func, original_instructions, original_vregs, original_block);

    free(nodes);
    free(map);
    free(ready);
    return valid;
}
