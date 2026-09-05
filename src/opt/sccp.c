/* Sparse conditional constant propagation over executable CFG edges. */
#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"

#include <stdlib.h>

typedef enum {
    SCCP_UNKNOWN,
    SCCP_CONSTANT,
    SCCP_OVERDEFINED,
} sccp_kind_t;

typedef struct {
    sccp_kind_t kind;
    anvil_value_t *constant;
} sccp_value_t;

typedef struct {
    anvil_ctx_t *ctx;
    anvil_opt_cfg_t cfg;
    anvil_def_use_t uses;
    sccp_value_t *values;
    bool *executable_blocks;
    bool *executable_edges;
    bool *queued;
    size_t *worklist;
    size_t *block_first;
    size_t *block_end;
    size_t head;
    size_t tail;
    size_t pending;
} sccp_state_t;

static sccp_value_t merge_values(sccp_value_t left, sccp_value_t right)
{
    if (left.kind == SCCP_UNKNOWN)
        return right;
    if (right.kind == SCCP_UNKNOWN)
        return left;
    if (left.kind == SCCP_CONSTANT && right.kind == SCCP_CONSTANT &&
        left.constant->data.u == right.constant->data.u && anvil_types_equal(left.constant->type, right.constant->type))
        return left;

    return (sccp_value_t){ .kind = SCCP_OVERDEFINED };
}

static sccp_value_t value_state(const sccp_state_t *state, anvil_value_t *value)
{
    if (value->kind == ANVIL_VAL_CONST_INT)
        return (sccp_value_t){ .kind = SCCP_CONSTANT, .constant = value };

    size_t definition = anvil_def_use_definition(&state->uses, value);
    if (definition != SIZE_MAX)
        return state->values[definition];

    return (sccp_value_t){ .kind = SCCP_OVERDEFINED };
}

static void enqueue(sccp_state_t *state, size_t instruction)
{
    if (state->queued[instruction])
        return;

    state->queued[instruction] = true;
    state->worklist[state->tail] = instruction;
    state->tail = (state->tail + 1) % state->uses.count;
    state->pending++;
}

static void activate_edge(sccp_state_t *state, size_t edge)
{
    if (state->executable_edges[edge])
        return;

    state->executable_edges[edge] = true;
    size_t target = state->cfg.successors[edge];
    bool was_executable = state->executable_blocks[target];
    state->executable_blocks[target] = true;
    for (size_t index = state->block_first[target]; index < state->block_end[target]; index++)
    {
        if (!was_executable || state->uses.instructions[index]->op == ANVIL_OP_PHI)
            enqueue(state, index);
    }
}

static bool edge_executable(const sccp_state_t *state, size_t source, size_t target)
{
    for (size_t edge = state->cfg.successor_offsets[source]; edge < state->cfg.successor_offsets[source + 1]; edge++)
    {
        if (state->cfg.successors[edge] == target)
            return state->executable_edges[edge];
    }

    return false;
}

static void visit_terminator(sccp_state_t *state, size_t block, const anvil_instr_t *instr, bool resolve_unknown)
{
    anvil_block_t *selected = NULL;
    if (instr->op == ANVIL_OP_BR)
    {
        selected = instr->true_block;
    }
    else if (instr->op == ANVIL_OP_BR_COND || instr->op == ANVIL_OP_SWITCH)
    {
        sccp_value_t condition = value_state(state, instr->operands[0]);
        if (condition.kind == SCCP_UNKNOWN && !resolve_unknown)
            return;

        if (condition.kind == SCCP_CONSTANT)
        {
            selected = instr->true_block;
            if (instr->op == ANVIL_OP_BR_COND)
            {
                selected = condition.constant->data.u ? instr->true_block : instr->false_block;
            }
            else
            {
                for (size_t index = 0; index < instr->num_switch_cases; index++)
                {
                    if (instr->operands[index + 1]->data.u == condition.constant->data.u)
                    {
                        selected = instr->switch_blocks[index];
                        break;
                    }
                }
            }
        }
    }

    for (size_t edge = state->cfg.successor_offsets[block]; edge < state->cfg.successor_offsets[block + 1]; edge++)
    {
        if (!selected || state->cfg.blocks[state->cfg.successors[edge]] == selected)
            activate_edge(state, edge);
    }
}

static sccp_value_t evaluate(sccp_state_t *state, size_t block, anvil_instr_t *instr)
{
    sccp_value_t unknown = { .kind = SCCP_UNKNOWN };
    sccp_value_t overdefined = { .kind = SCCP_OVERDEFINED };
    if (instr->result->type->kind < ANVIL_TYPE_I1 || instr->result->type->kind > ANVIL_TYPE_U64)
        return overdefined;

    if (instr->op == ANVIL_OP_PHI)
    {
        sccp_value_t value = unknown;
        for (size_t incoming = 0; incoming < instr->num_phi_incoming; incoming++)
        {
            size_t predecessor = anvil_opt_cfg_index(&state->cfg, instr->phi_blocks[incoming]);
            if (edge_executable(state, predecessor, block))
                value = merge_values(value, value_state(state, instr->operands[incoming]));
        }

        return value;
    }

    if (instr->op == ANVIL_OP_SELECT)
    {
        sccp_value_t condition = value_state(state, instr->operands[0]);
        if (condition.kind == SCCP_CONSTANT)
            return value_state(state, instr->operands[condition.constant->data.u ? 1 : 2]);

        return merge_values(value_state(state, instr->operands[1]), value_state(state, instr->operands[2]));
    }

    if (!instr->num_operands || instr->num_operands > 2)
        return overdefined;

    sccp_value_t left = value_state(state, instr->operands[0]);
    sccp_value_t right = instr->num_operands == 2 ? value_state(state, instr->operands[1]) : left;
    if (left.kind == SCCP_OVERDEFINED || right.kind == SCCP_OVERDEFINED)
        return overdefined;
    if (left.kind == SCCP_UNKNOWN || right.kind == SCCP_UNKNOWN)
        return unknown;

    anvil_value_t *folded = anvil_opt_fold_integer(state->ctx, instr, left.constant, right.constant);
    if (folded)
        return (sccp_value_t){ .kind = SCCP_CONSTANT, .constant = folded };

    return overdefined;
}

static void propagate(sccp_state_t *state)
{
    state->executable_blocks[state->cfg.entry] = true;
    for (size_t index = state->block_first[state->cfg.entry]; index < state->block_end[state->cfg.entry]; index++)
        enqueue(state, index);

    do
    {
        while (state->pending)
        {
            size_t index = state->worklist[state->head];
            state->head = (state->head + 1) % state->uses.count;
            state->pending--;
            state->queued[index] = false;
            anvil_instr_t *instr = state->uses.instructions[index];
            size_t block = anvil_opt_cfg_index(&state->cfg, instr->parent);
            if (!state->executable_blocks[block])
                continue;

            if (instr == instr->parent->last)
                visit_terminator(state, block, instr, false);
            if (!instr->result || state->values[index].kind == SCCP_OVERDEFINED)
                continue;

            sccp_value_t next = merge_values(state->values[index], evaluate(state, block, instr));
            if (anvil_ctx_get_last_error(state->ctx) != ANVIL_OK)
                return;
            if (next.kind == state->values[index].kind)
                continue;

            state->values[index] = next;
            for (size_t use = state->uses.use_offsets[index]; use < state->uses.use_offsets[index + 1]; use++)
                enqueue(state, state->uses.users[use]);
        }

        /* A remaining unresolved condition must not make a feasible edge
         * disappear. Activating it can expose more PHI information. */
        for (size_t block = 0; block < state->cfg.count; block++)
        {
            if (state->executable_blocks[block])
                visit_terminator(state, block, state->cfg.blocks[block]->last, true);
        }
    } while (state->pending);
}

static void destroy_state(sccp_state_t *state)
{
    anvil_opt_cfg_destroy(&state->cfg);
    anvil_def_use_destroy(&state->uses);
    free(state->values);
    free(state->executable_blocks);
    free(state->executable_edges);
    free(state->queued);
    free(state->worklist);
    free(state->block_first);
    free(state->block_end);
}

anvil_pass_result_t anvil_pass_sccp(anvil_func_t *func)
{
    if (!func || !func->parent)
        return ANVIL_PASS_RUN_ERROR;

    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
    if (func->is_declaration || !func->blocks)
        return ANVIL_PASS_RUN_UNCHANGED;

    sccp_state_t state = { .ctx = ctx };
    if (!anvil_opt_cfg_build(func, &state.cfg) || !anvil_def_use_build(func, &state.uses))
    {
        destroy_state(&state);
        return ANVIL_PASS_RUN_ERROR;
    }

    size_t edges = state.cfg.successor_offsets[state.cfg.count];
    state.values = anvil_ctx_calloc(ctx, state.uses.count, sizeof(*state.values));
    state.executable_blocks = anvil_ctx_calloc(ctx, state.cfg.count, sizeof(bool));
    state.executable_edges = anvil_ctx_calloc(ctx, edges ? edges : 1, sizeof(bool));
    state.queued = anvil_ctx_calloc(ctx, state.uses.count, sizeof(bool));
    state.worklist = anvil_ctx_calloc(ctx, state.uses.count, sizeof(size_t));
    state.block_first = anvil_ctx_calloc(ctx, state.cfg.count, sizeof(size_t));
    state.block_end = anvil_ctx_calloc(ctx, state.cfg.count, sizeof(size_t));
    if (!state.values || !state.executable_blocks || !state.executable_edges || !state.queued || !state.worklist || !state.block_first || !state.block_end)
    {
        destroy_state(&state);
        return ANVIL_PASS_RUN_ERROR;
    }

    for (size_t index = 0; index < state.uses.count; index++)
    {
        anvil_instr_t *instr = state.uses.instructions[index];
        size_t block = anvil_opt_cfg_index(&state.cfg, instr->parent);
        if (instr == instr->parent->first)
            state.block_first[block] = index;

        state.block_end[block] = index + 1;
    }

    propagate(&state);
    bool changed = false;
    if (anvil_ctx_get_last_error(ctx) == ANVIL_OK)
    {
        for (size_t index = 0; index < state.uses.count; index++)
        {
            if (state.values[index].kind != SCCP_CONSTANT)
                continue;

            anvil_instr_t *instr = state.uses.instructions[index];
            for (size_t use = state.uses.use_offsets[index]; use < state.uses.use_offsets[index + 1]; use++)
            {
                anvil_instr_t *user = state.uses.instructions[state.uses.users[use]];
                for (size_t operand = 0; operand < user->num_operands; operand++)
                {
                    if (user->operands[operand] == instr->result)
                        user->operands[operand] = state.values[index].constant;
                }
            }

            anvil_opt_erase_instr(instr);
            changed = true;
        }
    }

    destroy_state(&state);
    if (anvil_ctx_get_last_error(ctx) != ANVIL_OK)
        return ANVIL_PASS_RUN_ERROR;

    anvil_pass_result_t cleanup = anvil_pass_simplify_cfg(func);
    if (cleanup == ANVIL_PASS_RUN_ERROR)
        return cleanup;

    return changed || cleanup == ANVIL_PASS_RUN_CHANGED ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}
