/*
 * ANVIL - Dead Code Elimination Pass
 *
 * Mark and sweep, O(I + uses): the instructions that must run mark the
 * definitions they read, transitively; every other one goes, cycles of PHIs
 * and pure operations included.  Value IDs are context-global and may be
 * sparse, so they are deliberately not used as array indexes here.
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    anvil_instr_t *instr;
    bool live;
} dce_node_t;

typedef struct {
    anvil_value_t **keys;
    size_t *values;
    size_t cap;
} dce_map_t;

static size_t dce_ptr_hash(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    value ^= value >> 17;
    value *= UINT64_C(0xed5ad4bb);
    value ^= value >> 11;
    return (size_t)value;
}

static bool dce_map_find(const dce_map_t *map, anvil_value_t *key,
                         size_t *result)
{
    if (!map || !key || map->cap == 0) return false;
    size_t slot = dce_ptr_hash(key) & (map->cap - 1);
    for (size_t probe = 0; probe < map->cap; probe++) {
        if (!map->keys[slot]) return false;
        if (map->keys[slot] == key) {
            *result = map->values[slot];
            return true;
        }
        slot = (slot + 1) & (map->cap - 1);
    }
    return false;
}

static bool dce_map_insert(anvil_ctx_t *ctx, dce_map_t *map,
                           anvil_value_t *key, size_t value)
{
    size_t slot = dce_ptr_hash(key) & (map->cap - 1);
    for (size_t probe = 0; probe < map->cap; probe++) {
        if (!map->keys[slot]) {
            map->keys[slot] = key;
            map->values[slot] = value;
            return true;
        }
        if (map->keys[slot] == key) {
            anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                            "Two instructions define the same SSA value");
            return false;
        }
        slot = (slot + 1) & (map->cap - 1);
    }
    anvil_set_error(ctx, ANVIL_ERR_NOMEM, "DCE value map is full");
    return false;
}

/* Ordinary loads can fault and volatile loads are observable: a load stays
 * unless load_cannot_fault proves the first false and it is not volatile. */
static bool has_side_effects(const anvil_instr_t *instr)
{
    if (anvil_op_is_atomic(instr->op))
        return true;

    if (instr->op == ANVIL_OP_CALL)
        return anvil_opt_call_effects(instr) != 0;

    switch (instr->op) {
        case ANVIL_OP_SDIV:
        case ANVIL_OP_UDIV:
        case ANVIL_OP_SMOD:
        case ANVIL_OP_UMOD:
        case ANVIL_OP_LOAD:
        case ANVIL_OP_STORE:
        case ANVIL_OP_ALLOCA:
        case ANVIL_OP_BR:
        case ANVIL_OP_BR_COND:
        case ANVIL_OP_RET:
        case ANVIL_OP_SWITCH:
        case ANVIL_OP_FPTRUNC:
        case ANVIL_OP_FPEXT:
        case ANVIL_OP_FPTOSI:
        case ANVIL_OP_FPTOUI:
        case ANVIL_OP_SITOFP:
        case ANVIL_OP_UITOFP:
        case ANVIL_OP_FADD:
        case ANVIL_OP_FSUB:
        case ANVIL_OP_FMUL:
        case ANVIL_OP_FDIV:
        case ANVIL_OP_FCMP:
        case ANVIL_OP_FMA:
        case ANVIL_OP_FSQRT:
            return true;
        default:
            return false;
    }
}

/* A load that cannot fault: its whole range lies inside a stack object or a
 * global that exists (a weak declaration may resolve to nothing). */
static bool load_cannot_fault(const anvil_instr_t *instr)
{
    const anvil_value_t *object;
    size_t offset;
    if (instr->op != ANVIL_OP_LOAD || instr->memory_access.is_volatile || !instr->result || !instr->result->type || !instr->result->type->size ||
        !anvil_memory_bounded_range(instr->operands[0], instr->result->type->size, &object, &offset))
        return false;
    return object->kind != ANVIL_VAL_GLOBAL || !object->data.global.is_declaration || object->data.global.linkage != ANVIL_LINK_WEAK;
}

/* Whether the instruction runs whatever reads its result: effects, control
 * flow, and anything without a result but a call. */
static bool is_root(const anvil_instr_t *instr)
{
    if (instr->op == ANVIL_OP_NOP)
        return false;
    if (!instr->result && instr->op != ANVIL_OP_CALL)
        return true;
    return has_side_effects(instr) && !load_cannot_fault(instr);
}

anvil_pass_result_t anvil_pass_dce(anvil_func_t *func)
{
    if (!func || !func->parent || !func->parent->ctx)
        return ANVIL_PASS_RUN_ERROR;

    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);

    size_t count = 0;
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (count == SIZE_MAX) {
                anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                                "DCE instruction count overflow");
                return ANVIL_PASS_RUN_ERROR;
            }
            count++;
        }
    }
    if (count == 0) return ANVIL_PASS_RUN_UNCHANGED;
    if (count > SIZE_MAX / 2) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM, "DCE map capacity overflow");
        return ANVIL_PASS_RUN_ERROR;
    }

    size_t map_cap = 1;
    size_t required = count * 2;
    while (map_cap < required) {
        if (map_cap > SIZE_MAX / 2) {
            anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                            "DCE map capacity overflow");
            return ANVIL_PASS_RUN_ERROR;
        }
        map_cap *= 2;
    }

    dce_node_t *nodes = anvil_ctx_calloc(ctx, count, sizeof(*nodes));
    size_t *worklist = anvil_ctx_malloc(ctx, count * sizeof(*worklist));
    dce_map_t map = {
        .keys = anvil_ctx_calloc(ctx, map_cap, sizeof(*map.keys)),
        .values = anvil_ctx_malloc(ctx, map_cap * sizeof(*map.values)),
        .cap = map_cap
    };
    if (!nodes || !worklist || !map.keys || !map.values) goto fail;

    size_t node_index = 0;
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            nodes[node_index].instr = instr;
            if (instr->result &&
                !dce_map_insert(ctx, &map, instr->result, node_index)) {
                goto fail;
            }
            node_index++;
        }
    }

    size_t tail = 0;
    for (size_t i = 0; i < count; i++) {
        if (is_root(nodes[i].instr)) {
            nodes[i].live = true;
            worklist[tail++] = i;
        }
    }

    /* Values defined in this function only: constants, parameters and
     * globals have no node. */
    while (tail) {
        anvil_instr_t *instr = nodes[worklist[--tail]].instr;
        for (size_t op_index = 0; op_index < instr->num_operands; op_index++) {
            size_t definition;
            if (!dce_map_find(&map, instr->operands[op_index], &definition))
                continue;
            dce_node_t *operand_node = &nodes[definition];
            if (!operand_node->live && operand_node->instr->op != ANVIL_OP_NOP) {
                operand_node->live = true;
                worklist[tail++] = definition;
            }
        }
    }

    bool changed = false;
    for (size_t i = 0; i < count; i++) {
        if (!nodes[i].live && anvil_opt_erase_instr(nodes[i].instr))
            changed = true;
    }

    free(nodes);
    free(worklist);
    free(map.keys);
    free(map.values);
    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;

fail:
    free(nodes);
    free(worklist);
    free(map.keys);
    free(map.values);
    return ANVIL_PASS_RUN_ERROR;
}
