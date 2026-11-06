#include "ir_lower.h"
#include "mir_isel.h"
#include "parallel_move.h"
#include "switch_lower.h"
#include "wide_mir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t lane_operation_arity(anvil_op_t operation)
{
    switch (operation) {
    case ANVIL_OP_FADD:
    case ANVIL_OP_FSUB:
    case ANVIL_OP_FMUL:
    case ANVIL_OP_FDIV:
    case ANVIL_OP_ADD:
    case ANVIL_OP_SUB:
    case ANVIL_OP_MUL:
    case ANVIL_OP_AND:
    case ANVIL_OP_OR:
    case ANVIL_OP_XOR:
    case ANVIL_OP_SHL:
    case ANVIL_OP_SHR:
    case ANVIL_OP_SAR:
    case ANVIL_OP_CMP_EQ:
    case ANVIL_OP_CMP_NE:
    case ANVIL_OP_CMP_LT:
    case ANVIL_OP_CMP_LE:
    case ANVIL_OP_CMP_GT:
    case ANVIL_OP_CMP_GE:
    case ANVIL_OP_CMP_ULT:
    case ANVIL_OP_CMP_ULE:
    case ANVIL_OP_CMP_UGT:
    case ANVIL_OP_CMP_UGE:
    case ANVIL_OP_FCMP:
    case ANVIL_OP_UMULH:
    case ANVIL_OP_SMULH:
    case ANVIL_OP_ACCUM_ZEXT:
    case ANVIL_OP_ACCUM_SEXT:
    case ANVIL_OP_ADD_SAT_U:
    case ANVIL_OP_ADD_SAT_S:
    case ANVIL_OP_SUB_SAT_U:
    case ANVIL_OP_SUB_SAT_S:
    case ANVIL_OP_ROTL:
        return 2;
    case ANVIL_OP_SELECT:
    case ANVIL_OP_FMA:
        return 3;
    case ANVIL_OP_SPLAT:
    case ANVIL_OP_NEG:
    case ANVIL_OP_NOT:
    case ANVIL_OP_FNEG:
    case ANVIL_OP_FABS:
    case ANVIL_OP_FSQRT:
        return 1;
    default:
        return anvil_op_is_reduction(operation) ? 1 : 0;
    }
}

bool anvil_vector_mir_form(anvil_ctx_t *ctx, const anvil_instr_t *instr, anvil_vector_mir_form_t *form)
{
    size_t arity = lane_operation_arity(instr->op);
    bool reduction = anvil_op_is_reduction(instr->op);
    if (!instr->result || !arity)
        return false;
    /* Single values: only operations the target prices in the scalar
     * shape; every other scalar lowers through the backend's own forms. */
    if (instr->result->type->kind != ANVIL_TYPE_VECTOR && !reduction) {
        if (instr->num_operands != arity ||
            !anvil_vector_shape_cost(ctx, instr->op, instr->result->type, ANVIL_VECTOR_SHAPE_SCALAR))
            return false;
        *form = (anvil_vector_mir_form_t){.operation = instr->op, .lane_bits = (unsigned)instr->result->type->size * 8,
                                          .num_uses = arity, .supported = true, .scalar = true};
        return true;
    }

    /* A comparison or a lane-mask select is asked about (and runs on) the
     * vectors it compares or selects; its result is their mask. */
    bool select = instr->op == ANVIL_OP_SELECT;
    bool compare = instr->op == ANVIL_OP_FCMP || (instr->op >= ANVIL_OP_CMP_EQ && instr->op <= ANVIL_OP_CMP_UGE);
    if (select && instr->num_operands == 3 && instr->operands[0]->type->kind != ANVIL_TYPE_VECTOR)
        return false;
    anvil_type_t *type = instr->result->type;
    if (instr->num_operands == arity && (select || compare || reduction))
        type = instr->operands[select ? 1 : 0]->type;
    if (type->kind != ANVIL_TYPE_VECTOR)
        return false;
    *form = (anvil_vector_mir_form_t){.operation = instr->op, .predicate = instr->fcmp_pred,
                                      .lane_bits = (unsigned)type->data.vector.element->size * 8};
    if (instr->num_operands != arity)
        return true;
    form->num_uses = arity;
    form->widening = instr->op == ANVIL_OP_ACCUM_ZEXT || instr->op == ANVIL_OP_ACCUM_SEXT
                         ? anvil_vector_widening(instr->result->type, instr->operands[1]->type)
                         : 0;
    anvil_vector_shape_t shape = form->widening ? anvil_vector_widen_shape(form->widening)
                                 : arity == 2 && instr->operands[1]->type->kind != ANVIL_TYPE_VECTOR && instr->op != ANVIL_OP_SPLAT
                                     ? ANVIL_VECTOR_SHAPE_UNIFORM
                                     : ANVIL_VECTOR_SHAPE_LANES;
    form->supported = anvil_vector_shape_cost(ctx, instr->op, type, shape) != 0;
    return true;
}

bool anvil_vector_mir_emit(anvil_mir_func_t *mir, const anvil_vector_mir_form_t *form, anvil_mir_vreg_t def, const anvil_mir_vreg_t *uses)
{
    if (!form->supported || def == ANVIL_MIR_NO_VREG)
        return false;
    for (size_t use = 0; use < form->num_uses; use++) {
        if (uses[use] == ANVIL_MIR_NO_VREG)
            return false;
    }
    if (form->scalar)
        return anvil_mir_add_scalar(mir, form->operation, def, uses, form->num_uses, form->lane_bits);
    return anvil_mir_add_vector(mir, form->operation, def, uses, form->num_uses, form->lane_bits) &&
           (form->operation != ANVIL_OP_FCMP || anvil_mir_set_instr_fcmp_pred(mir, anvil_mir_num_instrs(mir) - 1, form->predicate)) &&
           (!form->widening || anvil_mir_set_instr_widening(mir, anvil_mir_num_instrs(mir) - 1, form->widening));
}

bool anvil_cast_keeps_value(const anvil_instr_t *instr)
{
    return instr->op == ANVIL_OP_BITCAST && instr->num_operands == 1 && instr->result && instr->result->type->kind == ANVIL_TYPE_PTR &&
           instr->operands[0]->type->kind == ANVIL_TYPE_PTR;
}

anvil_mir_block_t anvil_mir_block_for(anvil_mir_func_t *mir, const anvil_block_t *block, bool first)
{
    anvil_mir_block_t mir_block = first ? anvil_mir_current_block(mir) : anvil_mir_add_block(mir, block->name);
    if (mir_block != ANVIL_MIR_NO_BLOCK && !anvil_mir_set_block_cold(mir, mir_block, block->cold))
        return ANVIL_MIR_NO_BLOCK;
    return mir_block;
}

typedef struct {
    anvil_op_t ir;
    anvil_mir_opcode_t mir;
} ir_mir_op_t;

static const ir_mir_op_t binary_ops[] = {
    {ANVIL_OP_ADD, ANVIL_MIR_OP_ADD},       {ANVIL_OP_FADD, ANVIL_MIR_OP_ADD},       {ANVIL_OP_SUB, ANVIL_MIR_OP_SUB},
    {ANVIL_OP_FSUB, ANVIL_MIR_OP_SUB},      {ANVIL_OP_MUL, ANVIL_MIR_OP_MUL},        {ANVIL_OP_FMUL, ANVIL_MIR_OP_MUL},
    {ANVIL_OP_SDIV, ANVIL_MIR_OP_SDIV},     {ANVIL_OP_UDIV, ANVIL_MIR_OP_UDIV},      {ANVIL_OP_FDIV, ANVIL_MIR_OP_FDIV},
    {ANVIL_OP_SMOD, ANVIL_MIR_OP_SMOD},     {ANVIL_OP_UMOD, ANVIL_MIR_OP_UMOD},      {ANVIL_OP_AND, ANVIL_MIR_OP_AND},
    {ANVIL_OP_OR, ANVIL_MIR_OP_OR},         {ANVIL_OP_XOR, ANVIL_MIR_OP_XOR},        {ANVIL_OP_SHL, ANVIL_MIR_OP_SHL},
    {ANVIL_OP_SHR, ANVIL_MIR_OP_SHR},       {ANVIL_OP_SAR, ANVIL_MIR_OP_SAR},        {ANVIL_OP_CMP_EQ, ANVIL_MIR_OP_CMP_EQ},
    {ANVIL_OP_CMP_NE, ANVIL_MIR_OP_CMP_NE}, {ANVIL_OP_CMP_LT, ANVIL_MIR_OP_CMP_LT},  {ANVIL_OP_CMP_LE, ANVIL_MIR_OP_CMP_LE},
    {ANVIL_OP_CMP_GT, ANVIL_MIR_OP_CMP_GT}, {ANVIL_OP_CMP_GE, ANVIL_MIR_OP_CMP_GE},  {ANVIL_OP_CMP_ULT, ANVIL_MIR_OP_CMP_ULT},
    {ANVIL_OP_CMP_ULE, ANVIL_MIR_OP_CMP_ULE}, {ANVIL_OP_CMP_UGT, ANVIL_MIR_OP_CMP_UGT}, {ANVIL_OP_CMP_UGE, ANVIL_MIR_OP_CMP_UGE},
    {ANVIL_OP_FCMP, ANVIL_MIR_OP_FCMP},
};

static const ir_mir_op_t unary_ops[] = {
    {ANVIL_OP_NEG, ANVIL_MIR_OP_NEG}, {ANVIL_OP_FNEG, ANVIL_MIR_OP_NEG}, {ANVIL_OP_NOT, ANVIL_MIR_OP_NOT}, {ANVIL_OP_FABS, ANVIL_MIR_OP_FABS},
};

static const ir_mir_op_t cast_ops[] = {
    {ANVIL_OP_ZEXT, ANVIL_MIR_OP_ZEXT},       {ANVIL_OP_SEXT, ANVIL_MIR_OP_SEXT},       {ANVIL_OP_TRUNC, ANVIL_MIR_OP_TRUNC},
    {ANVIL_OP_BITCAST, ANVIL_MIR_OP_BITCAST}, {ANVIL_OP_PTRTOINT, ANVIL_MIR_OP_BITCAST}, {ANVIL_OP_INTTOPTR, ANVIL_MIR_OP_BITCAST},
    {ANVIL_OP_SITOFP, ANVIL_MIR_OP_SITOFP},   {ANVIL_OP_UITOFP, ANVIL_MIR_OP_UITOFP},   {ANVIL_OP_FPTOSI, ANVIL_MIR_OP_FPTOSI},
    {ANVIL_OP_FPTOUI, ANVIL_MIR_OP_FPTOUI},   {ANVIL_OP_FPEXT, ANVIL_MIR_OP_FPEXT},     {ANVIL_OP_FPTRUNC, ANVIL_MIR_OP_FPTRUNC},
};

static bool find_op(const ir_mir_op_t *table, size_t count, anvil_op_t op, anvil_mir_opcode_t *mir_op)
{
    for (size_t entry = 0; entry < count; entry++) {
        if (table[entry].ir == op) {
            *mir_op = table[entry].mir;
            return true;
        }
    }
    return false;
}

bool anvil_ir_binary_op(anvil_op_t op, anvil_mir_opcode_t *mir_op)
{
    return find_op(binary_ops, sizeof(binary_ops) / sizeof(binary_ops[0]), op, mir_op);
}

bool anvil_ir_unary_op(anvil_op_t op, anvil_mir_opcode_t *mir_op)
{
    return find_op(unary_ops, sizeof(unary_ops) / sizeof(unary_ops[0]), op, mir_op);
}

bool anvil_ir_cast_op(anvil_op_t op, anvil_mir_opcode_t *mir_op)
{
    return find_op(cast_ops, sizeof(cast_ops) / sizeof(cast_ops[0]), op, mir_op);
}

bool anvil_ir_compare_op(anvil_mir_opcode_t op)
{
    return op == ANVIL_MIR_OP_FCMP || anvil_mir_isel_is_compare(op);
}

const char *anvil_ir_call_symbol(const anvil_value_t *callee)
{
    if (callee && callee->kind == ANVIL_VAL_FUNC && callee->data.func)
        return callee->data.func->name;
    if (callee && callee->kind == ANVIL_VAL_GLOBAL && callee->type && callee->type->kind == ANVIL_TYPE_FUNC)
        return callee->name;
    return NULL;
}

bool anvil_ir_call_is_direct(const anvil_value_t *callee)
{
    return callee && (callee->kind == ANVIL_VAL_FUNC || (callee->kind == ANVIL_VAL_GLOBAL && callee->type && callee->type->kind == ANVIL_TYPE_FUNC));
}

anvil_type_t *anvil_ir_call_type(const anvil_value_t *callee)
{
    if (!callee || !callee->type)
        return NULL;
    if (callee->type->kind == ANVIL_TYPE_FUNC)
        return callee->type;
    anvil_type_t *pointee = callee->type->kind == ANVIL_TYPE_PTR ? callee->type->data.pointee : NULL;
    return pointee && pointee->kind == ANVIL_TYPE_FUNC ? pointee : NULL;
}

void anvil_ir_lower_destroy(anvil_ir_lower_t *lower)
{
    anvil_lower_map_destroy(&lower->values);
    anvil_lower_map_destroy(&lower->addresses);
    anvil_lower_map_destroy(&lower->blocks);
}

static bool starts_with_phi(const anvil_block_t *block)
{
    return block && block->first && block->first->op == ANVIL_OP_PHI;
}

/* The parallel copy an edge from `from` gives `to`'s PHIs, emitted into the
 * current block. */
static bool edge_copies(anvil_ir_lower_t *lower, const anvil_block_t *from, const anvil_block_t *to)
{
    anvil_mir_parallel_copy_t *copies = NULL;
    size_t count = 0, capacity = 0;
    bool ok = true;
    for (anvil_instr_t *phi = to->first; ok && phi && phi->op == ANVIL_OP_PHI; phi = phi->next) {
        anvil_value_t *incoming = NULL;
        for (size_t item = 0; item < phi->num_phi_incoming && item < phi->num_operands && !incoming; item++)
            if (phi->phi_blocks[item] == from)
                incoming = phi->operands[item];
        anvil_mir_vreg_t target[2], source[2];
        size_t words = 0, source_words = 0;
        ok = phi->result && incoming && lower->value_regs(lower, phi->result, target, &words) &&
             lower->value_regs(lower, incoming, source, &source_words) && words == source_words;
        for (size_t word = 0; ok && word < words; word++) {
            if (target[word] == source[word])
                continue;
            if (count == capacity) {
                size_t grown_capacity = capacity ? capacity * 2 : 4;
                anvil_mir_parallel_copy_t *grown = realloc(copies, grown_capacity * sizeof(*grown));
                ok = grown != NULL;
                if (!ok)
                    break;
                copies = grown;
                capacity = grown_capacity;
            }
            copies[count++] = (anvil_mir_parallel_copy_t){target[word], source[word]};
        }
    }
    ok = ok && anvil_mir_emit_parallel_copies(lower->mir, copies, count);
    free(copies);
    return ok;
}

static anvil_mir_block_t named_block(anvil_ir_lower_t *lower, const char *name)
{
    anvil_mir_block_t current = anvil_mir_current_block(lower->mir);
    anvil_mir_block_t block = anvil_mir_add_block(lower->mir, name);
    return block != ANVIL_MIR_NO_BLOCK && anvil_mir_set_current_block(lower->mir, current) ? block : ANVIL_MIR_NO_BLOCK;
}

anvil_mir_block_t anvil_ir_new_block(anvil_ir_lower_t *lower, const anvil_block_t *from, const char *what)
{
    char name[256];
    snprintf(name, sizeof(name), "%s_%s_%zu", from && from->name ? from->name : "anon", what, lower->num_edge_blocks++);
    return named_block(lower, name);
}

anvil_mir_block_t anvil_ir_edge_target(anvil_ir_lower_t *lower, anvil_block_t *from, anvil_block_t *to)
{
    anvil_mir_block_t target = anvil_lower_block_get(&lower->blocks, to);
    if (target == ANVIL_MIR_NO_BLOCK || !starts_with_phi(to))
        return target;
    char name[256];
    snprintf(name, sizeof(name), "%s_to_%s_phi_%zu", from && from->name ? from->name : "anon", to->name ? to->name : "anon", lower->num_edge_blocks++);
    anvil_mir_block_t edge = named_block(lower, name);
    anvil_mir_block_t current = anvil_mir_current_block(lower->mir);
    bool ok = edge != ANVIL_MIR_NO_BLOCK && anvil_mir_set_current_block(lower->mir, edge) && edge_copies(lower, from, to) &&
              anvil_mir_add_branch(lower->mir, target);
    return anvil_mir_set_current_block(lower->mir, current) && ok ? edge : ANVIL_MIR_NO_BLOCK;
}

bool anvil_ir_lower_branch(anvil_ir_lower_t *lower, const anvil_instr_t *instr)
{
    anvil_mir_block_t target = instr->true_block ? anvil_lower_block_get(&lower->blocks, instr->true_block) : ANVIL_MIR_NO_BLOCK;
    return target != ANVIL_MIR_NO_BLOCK && (!starts_with_phi(instr->true_block) || edge_copies(lower, instr->parent, instr->true_block)) &&
           anvil_mir_add_branch(lower->mir, target);
}

bool anvil_ir_lower_cond_branch(anvil_ir_lower_t *lower, const anvil_instr_t *instr)
{
    if (instr->num_operands != 1 || !instr->true_block || !instr->false_block)
        return false;
    anvil_mir_vreg_t condition = lower->value(lower, instr->operands[0]);
    anvil_mir_block_t on_true = anvil_ir_edge_target(lower, instr->parent, instr->true_block);
    anvil_mir_block_t on_false = on_true != ANVIL_MIR_NO_BLOCK ? anvil_ir_edge_target(lower, instr->parent, instr->false_block) : ANVIL_MIR_NO_BLOCK;
    return condition != ANVIL_MIR_NO_VREG && on_false != ANVIL_MIR_NO_BLOCK && anvil_mir_add_cond_branch(lower->mir, condition, on_true, on_false);
}

typedef struct {
    anvil_ir_lower_t *lower;
    const anvil_instr_t *instr;
} switch_user_t;

static anvil_mir_block_t switch_new_block(void *opaque)
{
    switch_user_t *user = opaque;
    return anvil_ir_new_block(user->lower, user->instr->parent, "switch_case");
}

static anvil_mir_block_t switch_case_target(void *opaque, size_t index)
{
    switch_user_t *user = opaque;
    return anvil_ir_edge_target(user->lower, user->instr->parent, user->instr->switch_blocks[index]);
}

static anvil_mir_vreg_t switch_bool(void *opaque)
{
    switch_user_t *user = opaque;
    return anvil_mir_add_vreg_typed(user->lower->mir, ANVIL_MIR_REG_GPR, 8, false);
}

/* The shared compare tree for a one-register 32- or 64-bit selector and
 * constant cases; -1 when the switch does not take it. */
static int switch_tree(anvil_ir_lower_t *lower, const anvil_instr_t *instr, anvil_mir_vreg_t selector, anvil_mir_block_t default_target)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(lower->mir, selector);
    anvil_type_t *type = instr->operands[0]->type;
    size_t count = instr->num_switch_cases;
    if (count < ANVIL_SWITCH_TREE_MIN_CASES || !info || info->reg_class != ANVIL_MIR_REG_GPR || (info->size_bits != 32 && info->size_bits != 64) ||
        !anvil_type_is_integer(type) || anvil_type_size(type) * 8 != info->size_bits)
        return -1;
    for (size_t i = 0; i < count; i++)
        if (instr->operands[i + 1]->kind != ANVIL_VAL_CONST_INT)
            return -1;
    int64_t *values = malloc(count * sizeof(*values));
    size_t *targets = malloc(count * sizeof(*targets));
    bool ok = values && targets;
    for (size_t i = 0; ok && i < count; i++) {
        values[i] = instr->operands[i + 1]->data.i;
        targets[i] = (size_t)(uintptr_t)instr->switch_blocks[i];
    }
    switch_user_t user = {lower, instr};
    anvil_switch_lowering_t lowering = {.mir = lower->mir, .user = &user, .selector = selector, .bits = info->size_bits, .is_signed = type->is_signed,
                                        .default_target = default_target, .new_block = switch_new_block, .case_target = switch_case_target,
                                        .new_bool = switch_bool};
    ok = ok && anvil_mir_lower_switch_tree(&lowering, values, targets, count);
    free(values);
    free(targets);
    return ok ? 1 : 0;
}

bool anvil_ir_lower_switch(anvil_ir_lower_t *lower, const anvil_instr_t *instr)
{
    if (instr->num_operands != instr->num_switch_cases + 1 || instr->num_operands < 1 || !instr->true_block)
        return false;
    anvil_mir_vreg_t selector[2];
    size_t words = 0;
    if (!lower->value_regs(lower, instr->operands[0], selector, &words))
        return false;
    anvil_mir_block_t default_target = anvil_ir_edge_target(lower, instr->parent, instr->true_block);
    if (default_target == ANVIL_MIR_NO_BLOCK)
        return false;
    if (instr->num_switch_cases == 0)
        return anvil_mir_add_branch(lower->mir, default_target);
    int tree = words == 1 ? switch_tree(lower, instr, selector[0], default_target) : -1;
    if (tree >= 0)
        return tree == 1;
    /* A chain: case i compares in its own block, falling to case i + 1; a
     * word pair compares as a pair. */
    for (size_t i = 0; i < instr->num_switch_cases; i++) {
        anvil_mir_block_t on_match = anvil_ir_edge_target(lower, instr->parent, instr->switch_blocks[i]);
        anvil_mir_block_t next = i + 1 < instr->num_switch_cases ? anvil_ir_new_block(lower, instr->parent, "switch_case") : default_target;
        anvil_mir_vreg_t value[2];
        size_t value_words = 0;
        anvil_mir_vreg_t match = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
        if (on_match == ANVIL_MIR_NO_BLOCK || next == ANVIL_MIR_NO_BLOCK || match == ANVIL_MIR_NO_VREG ||
            !lower->value_regs(lower, instr->operands[i + 1], value, &value_words) || value_words != words)
            return false;
        anvil_mir_vreg_t uses[] = {selector[0], value[0]};
        bool compared = words == 2 ? anvil_wide_compare(lower->mir, ANVIL_MIR_OP_CMP_EQ, (anvil_wide_pair_t){selector[0], selector[1]},
                                                        (anvil_wide_pair_t){value[0], value[1]}, match)
                                   : anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_CMP_EQ, match, uses, 2);
        if (!compared || !anvil_mir_add_cond_branch(lower->mir, match, on_match, next))
            return false;
        if (i + 1 < instr->num_switch_cases && !anvil_mir_set_current_block(lower->mir, next))
            return false;
    }
    return true;
}

bool anvil_ir_add_offset(anvil_ir_lower_t *lower, anvil_mir_vreg_t base, int64_t offset, anvil_mir_vreg_t *result)
{
    if (offset == 0) {
        *result = base;
        return true;
    }
    anvil_mir_vreg_t amount = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)lower->pointer_bits);
    anvil_mir_vreg_t sum = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)lower->pointer_bits);
    anvil_mir_vreg_t uses[] = {base, amount};
    if (amount == ANVIL_MIR_NO_VREG || sum == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, amount, offset) ||
        !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_ADD, sum, uses, 2))
        return false;
    *result = sum;
    return true;
}

bool anvil_ir_mark_symbol(anvil_ir_lower_t *lower, const anvil_value_t *source)
{
    anvil_ctx_t *ctx = lower->func->parent ? lower->func->parent->ctx : NULL;
    return !source || !anvil_symbol_uses_got(ctx, source) || anvil_mir_set_instr_symbol_got(lower->mir, anvil_mir_num_instrs(lower->mir) - 1, true);
}

/* A relocated constant (symbol + addend) carries its addend as the
 * instruction's immediate, 0 included. */
static anvil_mir_vreg_t symbol_address(anvil_ir_lower_t *lower, const char *symbol, bool relocated, int64_t addend, const anvil_value_t *source)
{
    if (!symbol || !symbol[0])
        return ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t vreg = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)lower->pointer_bits, false);
    bool made = vreg != ANVIL_MIR_NO_VREG && (relocated ? anvil_mir_add_instr_symbol_imm(lower->mir, ANVIL_MIR_OP_SYMBOL_ADDR, vreg, NULL, 0, symbol, addend)
                                                        : anvil_mir_add_instr_symbol(lower->mir, ANVIL_MIR_OP_SYMBOL_ADDR, vreg, NULL, 0, symbol));
    return made && anvil_ir_mark_symbol(lower, source) ? vreg : ANVIL_MIR_NO_VREG;
}

anvil_mir_vreg_t anvil_ir_symbol_address(anvil_ir_lower_t *lower, const char *symbol, const anvil_value_t *source)
{
    return symbol_address(lower, symbol, false, 0, source);
}

/* An FP constant's bits at its width (4 or 8 bytes). */
static int64_t float_bits(double value, size_t bytes)
{
    if (bytes == sizeof(float)) {
        float narrow = (float)value;
        uint32_t raw = 0;
        memcpy(&raw, &narrow, sizeof(raw));
        return (int64_t)raw;
    }
    uint64_t raw = 0;
    memcpy(&raw, &value, sizeof(raw));
    return (int64_t)raw;
}

static anvil_mir_vreg_t constant(anvil_ir_lower_t *lower, anvil_value_t *value)
{
    anvil_mir_vreg_t vreg = lower->vreg_for_type(lower, value->type);
    int64_t bits = value->kind == ANVIL_VAL_CONST_INT ? value->data.i : value->kind == ANVIL_VAL_CONST_FLOAT ? float_bits(value->data.f, value->type->size) : 0;
    return vreg != ANVIL_MIR_NO_VREG && anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, vreg, bits) ? vreg : ANVIL_MIR_NO_VREG;
}

anvil_mir_vreg_t anvil_ir_lower_value(anvil_ir_lower_t *lower, anvil_value_t *value)
{
    if (!value)
        return ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t existing = anvil_lower_vreg_get(&lower->values, value);
    if (existing != ANVIL_MIR_NO_VREG)
        return existing;
    anvil_mir_vreg_t base;
    int64_t offset;
    if (anvil_lower_address_get(&lower->addresses, value, &base, &offset)) {
        anvil_mir_vreg_t address;
        return anvil_ir_add_offset(lower, base, offset, &address) ? address : ANVIL_MIR_NO_VREG;
    }
    switch (value->kind) {
    case ANVIL_VAL_CONST_INT:
    case ANVIL_VAL_CONST_FLOAT:
    case ANVIL_VAL_CONST_NULL:
        return constant(lower, value);
    case ANVIL_VAL_CONST_STRING: {
        const char *label = NULL;
        return anvil_mir_add_string_literal(lower->mir, value->data.str, &label) < 0 ? ANVIL_MIR_NO_VREG : anvil_ir_symbol_address(lower, label, NULL);
    }
    case ANVIL_VAL_CONST_SYMBOL_ADDR:
    case ANVIL_VAL_CONST_GEP:
        return value->data.reloc.symbol
                   ? symbol_address(lower, value->data.reloc.symbol->name, true, value->data.reloc.addend, value->data.reloc.symbol)
                   : ANVIL_MIR_NO_VREG;
    case ANVIL_VAL_FUNC:
        return anvil_ir_symbol_address(lower, value->data.func && value->data.func->name ? value->data.func->name : value->name, value);
    case ANVIL_VAL_GLOBAL:
        return anvil_ir_symbol_address(lower, value->name, value);
    default:
        return ANVIL_MIR_NO_VREG;
    }
}

anvil_mir_vreg_t anvil_ir_resize(anvil_ir_lower_t *lower, anvil_mir_vreg_t value, uint16_t bits)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(lower->mir, value);
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR || !info->size_bits || !bits)
        return ANVIL_MIR_NO_VREG;
    if (info->size_bits == bits)
        return value;
    /* Read before adding a vreg (the array may move). */
    uint16_t from = info->size_bits;
    bool is_signed = info->is_signed;
    anvil_mir_vreg_t resized = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, bits, is_signed);
    anvil_mir_vreg_t uses[] = {value};
    anvil_mir_opcode_t op = from > bits ? ANVIL_MIR_OP_TRUNC : is_signed ? ANVIL_MIR_OP_SEXT : ANVIL_MIR_OP_ZEXT;
    return resized != ANVIL_MIR_NO_VREG && anvil_mir_add_instr(lower->mir, op, resized, uses, 1) ? resized : ANVIL_MIR_NO_VREG;
}

anvil_mir_vreg_t anvil_ir_pointer_width(anvil_ir_lower_t *lower, anvil_mir_vreg_t value, bool sign_extend)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(lower->mir, value);
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR)
        return ANVIL_MIR_NO_VREG;
    uint16_t bits = (uint16_t)lower->pointer_bits;
    if (info->size_bits == bits)
        return value;
    anvil_mir_opcode_t op = info->size_bits > bits ? ANVIL_MIR_OP_TRUNC : sign_extend ? ANVIL_MIR_OP_SEXT : ANVIL_MIR_OP_ZEXT;
    anvil_mir_vreg_t resized = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, bits, sign_extend);
    anvil_mir_vreg_t uses[] = {value};
    return resized != ANVIL_MIR_NO_VREG && anvil_mir_add_instr(lower->mir, op, resized, uses, 1) ? resized : ANVIL_MIR_NO_VREG;
}

bool anvil_ir_match_operand_sizes(anvil_ir_lower_t *lower, anvil_mir_vreg_t *lhs, anvil_mir_vreg_t *rhs, anvil_mir_vreg_t def)
{
    const anvil_mir_vreg_info_t *result = anvil_mir_get_vreg_info(lower->mir, def);
    const anvil_mir_vreg_info_t *left = anvil_mir_get_vreg_info(lower->mir, *lhs);
    const anvil_mir_vreg_info_t *right = anvil_mir_get_vreg_info(lower->mir, *rhs);
    if (!result || !left || !right)
        return false;
    if (result->reg_class != ANVIL_MIR_REG_GPR || left->reg_class != ANVIL_MIR_REG_GPR || right->reg_class != ANVIL_MIR_REG_GPR)
        return true;
    uint16_t bits = result->size_bits;
    *lhs = anvil_ir_resize(lower, *lhs, bits);
    *rhs = anvil_ir_resize(lower, *rhs, bits);
    return *lhs != ANVIL_MIR_NO_VREG && *rhs != ANVIL_MIR_NO_VREG;
}

/* A pointer in a register at the pointer width (a constant address
 * materialized: an address advanced by a constant stays an add of its own,
 * which update-form accesses fuse with the access after it). */
static anvil_mir_vreg_t pointer_register(anvil_ir_lower_t *lower, anvil_value_t *pointer)
{
    anvil_mir_vreg_t value = anvil_ir_lower_value(lower, pointer);
    return value != ANVIL_MIR_NO_VREG ? anvil_ir_pointer_width(lower, value, false) : ANVIL_MIR_NO_VREG;
}

bool anvil_ir_lower_gep(anvil_ir_lower_t *lower, const anvil_instr_t *instr)
{
    if (instr->num_operands < 2 || !instr->result || !instr->aux_type)
        return false;
    anvil_mir_vreg_t current = pointer_register(lower, instr->operands[0]);
    int64_t constant_offset = 0;
    if (current == ANVIL_MIR_NO_VREG)
        return false;
    anvil_type_t *walk = instr->aux_type;
    bool all_constant = true;
    uint16_t bits = (uint16_t)lower->pointer_bits;
    for (size_t i = 1; i < instr->num_operands; i++) {
        anvil_value_t *index_value = instr->operands[i];
        anvil_gep_step_t step;
        if (!anvil_gep_analyze_step(&walk, index_value, i - 1, &step) || step.amount > (size_t)INT64_MAX)
            return false;
        if (index_value->kind == ANVIL_VAL_CONST_INT) {
            int64_t offset;
            if (!anvil_gep_const_step_offset(&step, index_value, &offset) || !anvil_gep_accumulate_offset(&constant_offset, offset))
                return false;
            continue;
        }
        if (step.kind != ANVIL_GEP_STEP_SCALE)
            return false;
        all_constant = false;
        /* A word pair indexes by its low word: the address's width. */
        anvil_mir_vreg_t regs[2];
        size_t words = 0;
        if (!lower->value_regs(lower, index_value, regs, &words))
            return false;
        anvil_mir_vreg_t index = words == 2 ? regs[1] : anvil_ir_pointer_width(lower, regs[0], index_value->type->is_signed);
        anvil_mir_vreg_t scaled = index;
        if (index != ANVIL_MIR_NO_VREG && step.amount != 1) {
            anvil_mir_vreg_t scale = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, bits);
            scaled = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, bits);
            anvil_mir_vreg_t uses[] = {index, scale};
            if (scale == ANVIL_MIR_NO_VREG || scaled == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, scale, (int64_t)step.amount) ||
                !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_MUL, scaled, uses, 2))
                return false;
        }
        anvil_mir_vreg_t next = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, bits);
        anvil_mir_vreg_t uses[] = {current, scaled};
        if (scaled == ANVIL_MIR_NO_VREG || next == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_ADD, next, uses, 2))
            return false;
        current = next;
    }
    if (all_constant)
        return anvil_lower_address_put(&lower->addresses, instr->result, current, constant_offset);
    return anvil_ir_add_offset(lower, current, constant_offset, &current) && anvil_lower_vreg_put(&lower->values, instr->result, current);
}

bool anvil_ir_lower_struct_gep(anvil_ir_lower_t *lower, const anvil_instr_t *instr)
{
    if (instr->num_operands < 2 || !instr->result || !instr->aux_type || instr->aux_type->kind != ANVIL_TYPE_STRUCT ||
        instr->operands[1]->kind != ANVIL_VAL_CONST_INT)
        return false;
    uint64_t field = instr->operands[1]->data.u;
    anvil_mir_vreg_t base = field < instr->aux_type->data.struc.num_fields ? pointer_register(lower, instr->operands[0]) : ANVIL_MIR_NO_VREG;
    size_t offset = base != ANVIL_MIR_NO_VREG ? instr->aux_type->data.struc.offsets[field] : 0;
    return base != ANVIL_MIR_NO_VREG && offset <= INT64_MAX && anvil_lower_address_put(&lower->addresses, instr->result, base, (int64_t)offset);
}

bool anvil_ir_lower_memory_address(anvil_ir_lower_t *lower, anvil_value_t *pointer, anvil_mir_vreg_t *base, int64_t *displacement)
{
    if (anvil_lower_address_get(&lower->addresses, pointer, base, displacement)) {
        *base = anvil_ir_pointer_width(lower, *base, false);
        return *base != ANVIL_MIR_NO_VREG;
    }
    *displacement = 0;
    *base = pointer_register(lower, pointer);
    return *base != ANVIL_MIR_NO_VREG;
}
