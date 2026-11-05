/*
 * Switches that choose values become table lookups.
 *
 * A switch whose destinations only branch on to one merge block (or are
 * that block), where every PHI of the merge block takes a constant from
 * each of them, chooses values rather than code:
 *
 *     switch x [c0 -> e0, c1 -> e1, ...], default d
 *     ek: br merge             merge: v = phi [k0, e0], [k1, e1], ..., [kd, d]
 *
 * becomes, over the dense range [lo, hi] of the cases,
 *
 *     off = x - lo;  span = hi - lo + 1
 *     v = table_v[off u< span ? off : span]
 *     br merge
 *
 * with one read-only table per PHI, holes and the entry past the range
 * holding the default's value.  Destinations that each return a constant
 * (a classifying function) are the same choice: the block returns the
 * looked-up value.  A default that is real code keeps a branch
 * instead: off u< span ? lookup : d.  No unpredictable branch remains, on
 * any target.
 */
#include "anvil/anvil_analysis.h"
#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TABLE_MIN_CASES = 4,
    TABLE_MAX_SPAN = 4096,
    TABLE_MIN_DENSITY_PERCENT = 40,
    TABLE_MAX_PHIS = 8,
    TABLE_MAX_CASES = 1024,
};

/* The value `phi` takes from `block`, NULL when none. */
static anvil_value_t *incoming(const anvil_instr_t *phi, const anvil_block_t *block)
{
    for (size_t i = 0; i < phi->num_phi_incoming; i++)
        if (phi->phi_blocks[i] == block)
            return phi->operands[i];
    return NULL;
}

/* Whether no terminator but `from`'s names `block`. */
static bool entered_only_from(const anvil_func_t *func, const anvil_block_t *block, const anvil_block_t *from)
{
    for (const anvil_block_t *other = func->blocks; other; other = other->next) {
        const anvil_instr_t *term = other->last;
        if (!term || other == from)
            continue;
        if (term->true_block == block || term->false_block == block)
            return false;
        for (size_t c = 0; c < term->num_switch_cases; c++)
            if (term->switch_blocks[c] == block)
                return false;
    }
    return true;
}

/* Whether `block` only branches on to `merge`, entered from `from` alone. */
static bool forwards(const anvil_func_t *func, const anvil_block_t *block, const anvil_block_t *merge, const anvil_block_t *from)
{
    return block->first && block->first == block->last && block->first->op == ANVIL_OP_BR && block->first->true_block == merge &&
           entered_only_from(func, block, from);
}

typedef struct {
    anvil_instr_t *dispatch;
    anvil_block_t *block, *merge;
    anvil_instr_t *phis[TABLE_MAX_PHIS];
    size_t num_phis;
    bool default_value;          /* the default only forwards (or returns a constant) too */
    bool returns;                /* the destinations return constants: no merge block */
    uint64_t low, span;          /* in the selector's width */
} table_plan_t;

static uint64_t width_mask(unsigned bits)
{
    return bits >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << bits) - 1;
}

/* The value destination `target` gives `phi`: through its forwarding block
 * (or directly from the switch's block when it is the merge block). */
static anvil_value_t *chosen(const table_plan_t *plan, const anvil_instr_t *phi, anvil_block_t *target)
{
    if (plan->returns)
        return target->first->operands[0];
    return incoming(phi, target == plan->merge ? plan->block : target);
}

/* Whether `block` only returns a constant and is entered from `from` alone. */
static bool returns_constant(const anvil_func_t *func, const anvil_block_t *block, const anvil_block_t *from)
{
    const anvil_instr_t *ret = block->first;
    return ret && ret == block->last && ret->op == ANVIL_OP_RET && ret->num_operands == 1 && ret->operands[0]->kind == ANVIL_VAL_CONST_INT &&
           entered_only_from(func, block, from);
}

static bool recognize(anvil_func_t *func, anvil_block_t *block, table_plan_t *plan)
{
    anvil_instr_t *dispatch = block->last;
    if (!dispatch || dispatch->op != ANVIL_OP_SWITCH || dispatch->num_switch_cases < TABLE_MIN_CASES ||
        dispatch->num_switch_cases > TABLE_MAX_CASES ||
        dispatch->num_operands != dispatch->num_switch_cases + 1 || !anvil_type_is_integer(dispatch->operands[0]->type) ||
        dispatch->operands[0]->type->kind == ANVIL_TYPE_I1)
        return false;
    memset(plan, 0, sizeof(*plan));
    plan->dispatch = dispatch;
    plan->block = block;
    anvil_block_t *first = dispatch->switch_blocks[0], *fallback = dispatch->true_block;
    plan->returns = returns_constant(func, first, block) && anvil_type_is_integer(first->first->operands[0]->type);
    if (plan->returns) {
        /* Every case returns a constant; the default may be code. */
        for (size_t c = 0; c < dispatch->num_switch_cases; c++)
            if (!returns_constant(func, dispatch->switch_blocks[c], block))
                return false;
        plan->default_value = returns_constant(func, fallback, block);
        plan->num_phis = 1;
    } else {
        /* The merge block: where the first case's destination leads. */
        plan->merge = first->first && first->first == first->last && first->first->op == ANVIL_OP_BR ? first->first->true_block : first;
        if (!plan->merge || !plan->merge->first || plan->merge->first->op != ANVIL_OP_PHI)
            return false;
        for (size_t c = 0; c < dispatch->num_switch_cases; c++) {
            anvil_block_t *target = dispatch->switch_blocks[c];
            if (target != plan->merge && !forwards(func, target, plan->merge, block))
                return false;
        }
        plan->default_value = fallback == plan->merge || forwards(func, fallback, plan->merge, block);
    }
    /* Every PHI of the merge block takes a constant from each destination. */
    for (anvil_instr_t *phi = plan->returns ? NULL : plan->merge->first; phi && phi->op == ANVIL_OP_PHI; phi = phi->next) {
        if (plan->num_phis == TABLE_MAX_PHIS || !anvil_type_is_integer(phi->result->type))
            return false;
        for (size_t c = 0; c < dispatch->num_switch_cases; c++) {
            const anvil_value_t *value = chosen(plan, phi, dispatch->switch_blocks[c]);
            if (!value || value->kind != ANVIL_VAL_CONST_INT)
                return false;
        }
        const anvil_value_t *otherwise = plan->default_value ? chosen(plan, phi, fallback) : NULL;
        if (plan->default_value && (!otherwise || otherwise->kind != ANVIL_VAL_CONST_INT))
            return false;
        plan->phis[plan->num_phis++] = phi;
    }
    /* A dense range of case values. */
    unsigned bits = anvil_type_bit_width(dispatch->operands[0]->type);
    uint64_t mask = width_mask(bits), sign = UINT64_C(1) << (bits - 1);
    bool is_signed = dispatch->operands[0]->type->is_signed;
    uint64_t low = 0, high = 0;
    for (size_t c = 0; c < dispatch->num_switch_cases; c++) {
        uint64_t value = (uint64_t)dispatch->operands[c + 1]->data.i & mask;
        /* Ordered as the selector's type orders them: signed values biased. */
        uint64_t key = is_signed ? value ^ sign : value;
        if (c == 0 || key < low)
            low = key;
        if (c == 0 || key > high)
            high = key;
    }
    uint64_t span = high - low + 1;
    if (span > TABLE_MAX_SPAN || dispatch->num_switch_cases * 100 < span * TABLE_MIN_DENSITY_PERCENT)
        return false;
    plan->low = is_signed ? low ^ sign : low;
    plan->span = span;
    return true;
}

static anvil_instr_t *make(anvil_ctx_t *ctx, anvil_instr_t *before, anvil_op_t op, anvil_type_t *type, anvil_value_t *const *operands, size_t count)
{
    anvil_instr_t *instr = anvil_instr_create(ctx, op, type, "table");
    if (!instr || !anvil_instr_add_operands(instr, operands, count))
        return NULL;
    anvil_opt_insert_before(before, instr);
    return instr;
}

/* Everything new is built before the switch goes; on a failed allocation
 * the unused instructions stay behind, valid IR. */
static anvil_pass_result_t rewrite(anvil_func_t *func, table_plan_t *plan)
{
    anvil_ctx_t *ctx = func->owner_ctx;
    anvil_instr_t *dispatch = plan->dispatch;
    anvil_value_t *selector = dispatch->operands[0];
    anvil_type_t *type = selector->type;
    unsigned bits = anvil_type_bit_width(type);
    anvil_type_t *unsigned_type = bits <= 8 ? anvil_type_u8(ctx) : bits <= 16 ? anvil_type_u16(ctx) : bits <= 32 ? anvil_type_u32(ctx)
                                                                                                       : anvil_type_u64(ctx);
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(ctx);
    anvil_type_t *index_type = arch && arch->ptr_size == 4 ? anvil_type_u32(ctx) : anvil_type_u64(ctx);

    /* The lookup block, where the default is code: in ? lookup : default. */
    anvil_block_t *lookup = NULL;
    anvil_instr_t *anchor = dispatch;
    if (!plan->default_value) {
        char name[64];
        snprintf(name, sizeof(name), "%s.lookup", plan->block->name ? plan->block->name : "switch");
        lookup = anvil_block_prepare(func, name);
        if (!lookup)
            return ANVIL_PASS_RUN_ERROR;
    }

    /* off = x - lo (unsigned), in = off u< span. */
    anvil_value_t *low = anvil_opt_make_const_int(ctx, type, (int64_t)plan->low);
    anvil_value_t *sub_ops[] = {selector, low};
    anvil_instr_t *offset = low ? make(ctx, anchor, ANVIL_OP_SUB, type, sub_ops, 2) : NULL;
    anvil_instr_t *as_unsigned = offset && unsigned_type != type ? make(ctx, anchor, ANVIL_OP_BITCAST, unsigned_type, &offset->result, 1) : offset;
    anvil_value_t *span = anvil_opt_make_const_int(ctx, unsigned_type, (int64_t)plan->span);
    anvil_value_t *in_ops[] = {as_unsigned ? as_unsigned->result : NULL, span};
    anvil_instr_t *in_range = as_unsigned && span ? make(ctx, anchor, ANVIL_OP_CMP_ULT, ctx->type_i1, in_ops, 2) : NULL;
    if (!in_range)
        return ANVIL_PASS_RUN_ERROR;
    anvil_value_t *index = as_unsigned->result;
    if (!anvil_types_equal(unsigned_type, index_type)) {
        anvil_op_t op = anvil_type_bit_width(unsigned_type) < anvil_type_bit_width(index_type) ? ANVIL_OP_ZEXT : ANVIL_OP_TRUNC;
        anvil_instr_t *wide = make(ctx, anchor, op, index_type, &index, 1);
        if (!wide)
            return ANVIL_PASS_RUN_ERROR;
        index = wide->result;
    }
    if (plan->default_value) {
        /* Out of the range: the entry past it, the default's value. */
        anvil_value_t *past = anvil_opt_make_const_int(ctx, index_type, (int64_t)plan->span);
        anvil_value_t *clamp_ops[] = {in_range->result, index, past};
        anvil_instr_t *clamped = past ? make(ctx, anchor, ANVIL_OP_SELECT, index_type, clamp_ops, 3) : NULL;
        if (!clamped)
            return ANVIL_PASS_RUN_ERROR;
        index = clamped->result;
    }

    /* One table per PHI, loaded where the value is chosen. */
    anvil_value_t *values[TABLE_MAX_PHIS];
    anvil_instr_t *lookup_branch = NULL;
    if (lookup) {
        /* Its terminator: on to the merge block, or the return (its value
         * set once the load exists). */
        lookup_branch = anvil_instr_create(ctx, plan->returns ? ANVIL_OP_RET : ANVIL_OP_BR, ctx->type_void, NULL);
        if (!lookup_branch || (plan->returns && !anvil_instr_reserve_operands(lookup_branch, 1)))
            return ANVIL_PASS_RUN_ERROR;
        lookup_branch->true_block = plan->returns ? NULL : plan->merge;
        anvil_block_append_prepared(lookup, lookup_branch);
    }
    anvil_instr_t *at = lookup ? lookup_branch : anchor;
    for (size_t p = 0; p < plan->num_phis; p++) {
        anvil_instr_t *phi = plan->phis[p];
        anvil_type_t *element = plan->returns ? dispatch->switch_blocks[0]->first->operands[0]->type : phi->result->type;
        uint64_t length = plan->span + (plan->default_value ? 1 : 0);
        anvil_value_t **entries = calloc(length, sizeof(*entries));
        if (!entries)
            return ANVIL_PASS_RUN_ERROR;
        anvil_value_t *otherwise = plan->default_value ? chosen(plan, phi, dispatch->true_block) : NULL;
        anvil_value_t *filler = otherwise ? otherwise : anvil_opt_make_const_int(ctx, element, 0);
        for (uint64_t k = 0; k < length; k++)
            entries[k] = filler;
        unsigned bits_mask = bits;
        /* Later cases repeating a value do not override the first. */
        bool *set = calloc(plan->span, sizeof(*set));
        if (!set || !filler) {
            free(entries);
            free(set);
            return ANVIL_PASS_RUN_ERROR;
        }
        for (size_t c = 0; c < dispatch->num_switch_cases; c++) {
            uint64_t k = ((uint64_t)dispatch->operands[c + 1]->data.i - plan->low) & width_mask(bits_mask);
            if (k < plan->span && !set[k]) {
                entries[k] = chosen(plan, phi, dispatch->switch_blocks[c]);
                set[k] = true;
            }
        }
        free(set);
        char name[160];
        snprintf(name, sizeof(name), "%s.switch.%u.%zu", func->name ? func->name : "f", (unsigned)plan->block->id, p);
        anvil_type_t *array = anvil_type_array(ctx, element, length);
        anvil_value_t *table = array ? anvil_module_add_global(func->parent, name, array, ANVIL_LINK_INTERNAL) : NULL;
        anvil_value_t *init = table ? anvil_const_array(ctx, element, entries, length) : NULL;
        free(entries);
        anvil_value_t *zero = anvil_opt_make_const_int(ctx, index_type, 0);
        if (!init || !zero || !anvil_global_set_initializer(table, init) || !anvil_global_set_constant(table, true))
            return ANVIL_PASS_RUN_ERROR;
        anvil_value_t *gep_ops[] = {table, zero, index};
        anvil_instr_t *address = make(ctx, at, ANVIL_OP_GEP, anvil_type_ptr(ctx, element), gep_ops, 3);
        if (address)
            address->aux_type = array;
        anvil_instr_t *load = address ? make(ctx, at, ANVIL_OP_LOAD, element, &address->result, 1) : NULL;
        if (!load)
            return ANVIL_PASS_RUN_ERROR;
        values[p] = load->result;
    }
    /* Room in each PHI for the new edge, before anything changes. */
    anvil_block_t *from = lookup ? lookup : plan->block;
    for (size_t p = 0; p < plan->num_phis && !plan->returns; p++)
        if (!anvil_instr_reserve_operands(plan->phis[p], plan->phis[p]->num_operands + 1))
            return ANVIL_PASS_RUN_ERROR;
    for (size_t p = 0; p < plan->num_phis && !plan->returns; p++) {
        anvil_instr_t *phi = plan->phis[p];
        if (phi->phi_capacity < phi->num_phi_incoming + 1) {
            anvil_block_t **blocks = anvil_ctx_malloc(ctx, (phi->num_phi_incoming + 1) * sizeof(*blocks));
            if (!blocks)
                return ANVIL_PASS_RUN_ERROR;
            memcpy(blocks, phi->phi_blocks, phi->num_phi_incoming * sizeof(*blocks));
            free(phi->phi_blocks);
            phi->phi_blocks = blocks;
            phi->phi_capacity = phi->num_phi_incoming + 1;
        }
    }

    anvil_block_t *fallback = dispatch->true_block;
    anvil_op_t ending = lookup ? ANVIL_OP_BR_COND : plan->returns ? ANVIL_OP_RET : ANVIL_OP_BR;
    anvil_instr_t *branch = anvil_instr_create(ctx, ending, ctx->type_void, NULL);
    if (!branch || (lookup && !anvil_instr_add_operand(branch, in_range->result)) ||
        (ending == ANVIL_OP_RET && !anvil_instr_add_operand(branch, values[0])))
        return ANVIL_PASS_RUN_ERROR;

    /* Commit, allocating nothing: the switch becomes a branch, the merge
     * block's PHIs take the chosen values from it, and the destinations
     * that only forwarded (reached from the switch alone) go with their
     * PHI entries. */
    anvil_block_t *gone[2 * TABLE_MAX_CASES + 2];
    size_t num_gone = 0;
    for (size_t c = 0; c <= dispatch->num_switch_cases; c++) {
        anvil_block_t *target = c < dispatch->num_switch_cases ? dispatch->switch_blocks[c] : fallback;
        bool forwarded = (plan->returns || target != plan->merge) && (c < dispatch->num_switch_cases || plan->default_value);
        for (size_t g = 0; g < num_gone && forwarded; g++)
            forwarded = gone[g] != target;
        if (forwarded)
            gone[num_gone++] = target;
    }
    if (plan->returns && lookup)
        lookup_branch->operands[lookup_branch->num_operands++] = values[0];
    for (size_t p = 0; p < plan->num_phis && !plan->returns; p++) {
        anvil_instr_t *phi = plan->phis[p];
        size_t kept = 0;
        for (size_t i = 0; i < phi->num_phi_incoming; i++) {
            bool drop = phi->phi_blocks[i] == plan->block;
            for (size_t g = 0; g < num_gone && !drop; g++)
                drop = phi->phi_blocks[i] == gone[g];
            if (drop)
                continue;
            phi->phi_blocks[kept] = phi->phi_blocks[i];
            phi->operands[kept++] = phi->operands[i];
        }
        phi->phi_blocks[kept] = from;
        phi->operands[kept++] = values[p];
        phi->num_phi_incoming = phi->num_operands = kept;
    }
    branch->true_block = lookup ? lookup : plan->returns ? NULL : plan->merge;
    branch->false_block = lookup ? fallback : NULL;
    anvil_opt_insert_before(dispatch, branch);
    anvil_opt_erase_instr(dispatch);
    for (size_t g = 0; g < num_gone; g++)
        anvil_func_unlink_block(func, gone[g]);
    if (lookup)
        anvil_func_append_block(func, lookup);
    anvil_func_invalidate_cfg(func);
    return ANVIL_PASS_RUN_CHANGED;
}

anvil_pass_result_t anvil_pass_switch_table(anvil_func_t *func)
{
    if (!func || !func->parent)
        return ANVIL_PASS_RUN_ERROR;
    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        table_plan_t plan;
        if (!recognize(func, block, &plan))
            continue;
        anvil_pass_result_t status = rewrite(func, &plan);
        if (status == ANVIL_PASS_RUN_ERROR)
            return status;
        result = ANVIL_PASS_RUN_CHANGED;
    }
    return result;
}
