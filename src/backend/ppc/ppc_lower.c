#include "../common/lower_map.h"
#include "../common/switch_lower.h"
#include "ppc_internal.h"
#include "../common/narrow_mir.h"
#include "../common/wide_mir.h"

typedef struct {
    anvil_ir_lower_t base;
    const anvil_ppc_target_desc_t *desc;
    anvil_lower_map_t wide_consts;
    anvil_lower_map_t wide_pairs;
    /* SRET result returned in registers: RET loads it from this object. */
    bool sret_in_regs;
    ppc_agg_plan_t sret_plan;
    anvil_mir_vreg_t sret_object;
    /* Argument state after the named parameters (variadic callees). */
    ppc_arg_state_t named_args;
} ppc_mir_lower_t;

static void ppc_lower_free(ppc_mir_lower_t *lower)
{
    if (!lower)
        return;
    anvil_ir_lower_destroy(&lower->base);
    anvil_lower_map_destroy(&lower->wide_consts);
    anvil_lower_map_destroy(&lower->wide_pairs);
}

static bool ppc_type_is_fp(anvil_type_t *type)
{
    return type && (type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64);
}

static bool ppc_type_is_void(anvil_type_t *type)
{
    return !type || type->kind == ANVIL_TYPE_VOID;
}

static bool ppc_type_is_signed(anvil_type_t *type)
{
    return type && type->is_signed && !ppc_type_is_fp(type);
}

static bool ppc_type_is_64bit_integer(anvil_type_t *type)
{
    return type && (type->kind == ANVIL_TYPE_I64 || type->kind == ANVIL_TYPE_U64);
}

static bool ppc_needs_i64_pair(ppc_mir_lower_t *lower, anvil_type_t *type)
{
    return lower && lower->desc && lower->desc->word_size == 4 && ppc_type_is_64bit_integer(type);
}

static bool ppc_ensure_i64_pair(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *hi, anvil_mir_vreg_t *lo, bool *is_unsigned);

static anvil_mir_reg_class_t ppc_reg_class_for_type(anvil_type_t *type)
{
    if (type && type->kind == ANVIL_TYPE_VECTOR)
        return ANVIL_MIR_REG_VECTOR;
    return ppc_type_is_fp(type) ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR;
}

static uint16_t ppc_bits_for_type(const anvil_ppc_target_desc_t *desc, anvil_type_t *type)
{
    if (!desc)
        return 64;
    if (!type || type->kind == ANVIL_TYPE_PTR)
        return (uint16_t)(desc->word_size * 8);

    size_t size = anvil_type_size(type);
    if (size == 0)
        return (uint16_t)(desc->word_size * 8);
    if (size > UINT16_MAX / 8)
        return (uint16_t)(desc->word_size * 8);
    return (uint16_t)(size * 8);
}

static size_t ppc_slot_bytes_for_type(const anvil_ppc_target_desc_t *desc, anvil_type_t *type)
{
    size_t size = type ? anvil_type_size(type) : desc->word_size;
    return size ? size : desc->word_size;
}

static uint16_t ppc_align_for_type(const anvil_ppc_target_desc_t *desc, anvil_type_t *type)
{
    size_t align = type ? anvil_type_align(type) : desc->word_size;
    if (align == 0)
        align = desc->word_size;
    if (align > desc->word_size)
        align = desc->word_size;
    if (align > UINT16_MAX)
        align = UINT16_MAX;
    return (uint16_t)align;
}

static anvil_mir_vreg_t ppc_add_vreg_for_type(ppc_mir_lower_t *lower, anvil_type_t *type)
{
    /* Vectors live in VSX registers, only where the CPU has them. */
    if (type && type->kind == ANVIL_TYPE_VECTOR && !anvil_vector_operation_cost(lower->base.func->owner_ctx, ANVIL_OP_LOAD, type))
        return ANVIL_MIR_NO_VREG;
    return anvil_mir_add_vreg_typed(lower->base.mir, ppc_reg_class_for_type(type), ppc_bits_for_type(lower->desc, type), ppc_type_is_signed(type));
}

/* ------------------------------------------------------------------
 * Argument passing.  Where each piece goes is the ABI module's decision
 * (abi_ops->assign_*); this code only moves values there.
 * ------------------------------------------------------------------ */


static ppc_arg_loc_t ppc_assign_word(const ppc_mir_lower_t *lower, ppc_arg_state_t *state)
{
    return lower->desc->abi_ops->assign_word(lower->desc, state);
}

static ppc_arg_loc_t ppc_assign_fpr(const ppc_mir_lower_t *lower, ppc_arg_state_t *state, bool single)
{
    return lower->desc->abi_ops->assign_fpr(lower->desc, state, single);
}

static anvil_agg_target_t ppc_agg_target(const ppc_mir_lower_t *lower)
{
    return ppc_agg_target_of(lower->desc);
}

/* Doubleword `index` of an aggregate's parameter image.  A whole aggregate
 * smaller than a doubleword is right-justified on big-endian targets. */
static anvil_agg_part_t ppc_image_part(const ppc_mir_lower_t *lower, const ppc_agg_plan_t *plan, size_t index)
{
    anvil_agg_target_t target = ppc_agg_target(lower);
    anvil_agg_part_t part = anvil_agg_chunk(plan->size, index * target.word, target);
    if (plan->size < target.word && target.big_endian)
        part.pad = target.word - part.size;
    return part;
}

static size_t ppc_image_words(const ppc_mir_lower_t *lower, const ppc_agg_plan_t *plan)
{
    return (plan->size + lower->desc->word_size - 1) / lower->desc->word_size;
}

/* Leading members of a homogeneous floating aggregate that still get FPRs:
 * ELFv2 passes as many as fit, the rest in the doublewords of the image
 * from the first member left over (GPRs, then the stack). */
static unsigned ppc_hfa_parts_in_fprs(const ppc_mir_lower_t *lower, const ppc_agg_plan_t *plan, const ppc_arg_state_t *state)
{
    size_t left = state->fpr < lower->desc->num_fpr_arg_regs ? lower->desc->num_fpr_arg_regs - state->fpr : 0;
    return plan->num_parts < left ? plan->num_parts : (unsigned)left;
}

/* An incoming argument as an ordinary virtual register.  Narrow integers in
 * memory are read as their whole (right-justified) word. */
static anvil_mir_vreg_t ppc_incoming_value(ppc_mir_lower_t *lower, ppc_arg_loc_t loc, anvil_mir_reg_class_t cls, uint16_t bits, bool is_signed)
{
    anvil_mir_vreg_t value = anvil_mir_add_vreg_typed(lower->base.mir, cls, bits, is_signed);
    if (value == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;

    if (loc.reg >= 0) {
        anvil_mir_vreg_t incoming = anvil_mir_add_vreg_typed(lower->base.mir, cls, bits, is_signed);
        anvil_mir_vreg_t uses[] = {incoming};
        if (incoming == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->base.mir, incoming, loc.reg) || !anvil_mir_set_live_in(lower->base.mir, incoming, true) ||
            !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, value, uses, 1))
            return ANVIL_MIR_NO_VREG;
        return value;
    }

    uint16_t word_bits = (uint16_t)(lower->desc->word_size * 8);
    if (cls == ANVIL_MIR_REG_GPR && bits < word_bits) {
        anvil_mir_vreg_t word = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, word_bits, is_signed);
        if (word == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, word, loc.offset))
            return ANVIL_MIR_NO_VREG;
        return anvil_ir_resize(&lower->base, word, bits);
    }
    return anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, value, loc.offset) ? value : ANVIL_MIR_NO_VREG;
}

/* Fills the callee's copy of a BYVAL argument. */
static bool lower_incoming_aggregate(ppc_mir_lower_t *lower, const ppc_agg_plan_t *plan, anvil_mir_vreg_t object, ppc_arg_state_t *state)
{
    const anvil_ppc_target_desc_t *desc = lower->desc;
    anvil_agg_target_t target = ppc_agg_target(lower);
    size_t first_word = 0;
    if (plan->placement == PPC_AGG_REGS) {
        unsigned in_fprs = ppc_hfa_parts_in_fprs(lower, plan, state);
        for (unsigned part = 0; part < in_fprs; part++) {
            const anvil_agg_part_t *p = &plan->parts[part];
            ppc_arg_loc_t loc = {desc->fpr_arg_regs[state->fpr++], 0};
            anvil_mir_vreg_t value = ppc_incoming_value(lower, loc, ANVIL_MIR_REG_FPR, anvil_agg_part_bits(p, target), false);
            if (value == ANVIL_MIR_NO_VREG || !anvil_agg_store_part(lower->base.mir, p, value, object, target))
                return false;
        }
        if (in_fprs == plan->num_parts) {
            state->stack += (int64_t)ppc_image_words(lower, plan) * 8;
            return true;
        }
        /* The members past the last FPR arrive in their own doublewords. */
        state->fpr = desc->num_fpr_arg_regs;
        first_word = plan->parts[in_fprs].offset / desc->word_size;
        state->stack += (int64_t)first_word * 8;
    }

    if (plan->align >= 16 && first_word == 0)
        state->stack = (state->stack + 15) & ~INT64_C(15);
    for (size_t index = first_word; index < ppc_image_words(lower, plan); index++) {
        anvil_agg_part_t part = ppc_image_part(lower, plan, index);
        anvil_mir_vreg_t value = ppc_incoming_value(lower, ppc_assign_word(lower, state), ANVIL_MIR_REG_GPR, (uint16_t)(target.word * 8), false);
        if (value == ANVIL_MIR_NO_VREG || !anvil_agg_store_part(lower->base.mir, &part, value, object, target))
            return false;
    }
    return true;
}

static bool lower_params(ppc_mir_lower_t *lower)
{
    ppc_arg_state_t state = {0, 0, 0};

    anvil_mir_block_t entry = anvil_lower_block_get(&lower->base.blocks, lower->base.func->blocks);
    if (entry == ANVIL_MIR_NO_BLOCK || !anvil_mir_set_current_block(lower->base.mir, entry)) {
        return false;
    }

    for (size_t i = 0; i < lower->base.func->num_params; i++) {
        anvil_value_t *param = lower->base.func->params[i];
        if (!param)
            return false;

        anvil_param_attr_t attr = anvil_type_func_param_attr(lower->base.func->type, i);
        if (attr != ANVIL_PARAM_PLAIN) {
            ppc_agg_plan_t plan;
            if (!lower->desc->abi_ops->classify_aggregate(param->type->data.pointee, attr == ANVIL_PARAM_SRET, &plan))
                return false;
            if (plan.placement == PPC_AGG_REGS || plan.placement == PPC_AGG_IMAGE) {
                /* The callee's own copy of the argument, or the local
                 * result object RET loads into the return registers. */
                anvil_mir_vreg_t object = anvil_agg_frame_object(lower->base.mir, plan.size, plan.align, ppc_agg_target(lower));
                if (object == ANVIL_MIR_NO_VREG || !anvil_lower_vreg_put(&lower->base.values, param, object))
                    return false;
                if (attr == ANVIL_PARAM_SRET) {
                    lower->sret_in_regs = true;
                    lower->sret_plan = plan;
                    lower->sret_object = object;
                } else if (!lower_incoming_aggregate(lower, &plan, object, &state)) {
                    return false;
                }
                continue;
            }
            /* PPC_AGG_REFERENCE / PPC_AGG_INDIRECT: an ordinary pointer. */
        }

        if (ppc_needs_i64_pair(lower, param->type)) {
            bool is_unsigned = param->type->kind == ANVIL_TYPE_U64;
            int lo_reg = -1;
            if (!lower->desc->abi_ops->assign_pair)
                return false;
            ppc_arg_loc_t loc = lower->desc->abi_ops->assign_pair(lower->desc, &state, &lo_reg);
            ppc_arg_loc_t lo_loc = {lo_reg, loc.offset + 4};
            anvil_mir_vreg_t hi = ppc_incoming_value(lower, loc, ANVIL_MIR_REG_GPR, 32, !is_unsigned);
            anvil_mir_vreg_t lo = ppc_incoming_value(lower, lo_loc, ANVIL_MIR_REG_GPR, 32, false);
            if (hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG || !anvil_lower_pair_put(&lower->wide_pairs, param, hi, lo, is_unsigned))
                return false;
            continue;
        }

        ppc_arg_loc_t loc = ppc_type_is_fp(param->type) ? ppc_assign_fpr(lower, &state, param->type->kind == ANVIL_TYPE_F32) : ppc_assign_word(lower, &state);
        anvil_mir_vreg_t value = ppc_incoming_value(lower, loc, ppc_reg_class_for_type(param->type), ppc_bits_for_type(lower->desc, param->type), ppc_type_is_signed(param->type));
        if (value == ANVIL_MIR_NO_VREG || !anvil_lower_vreg_put(&lower->base.values, param, value))
            return false;
    }

    lower->named_args = state;
    return true;
}

static bool ppc_lower_wide_binary(ppc_mir_lower_t *lower, anvil_instr_t *instr, anvil_mir_opcode_t mir_op);
static bool ppc_ensure_wide(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_wide_pair_t *pair);
static anvil_wide_target_t ppc_wide_target(ppc_mir_lower_t *lower);

static bool prepare_phi_results(ppc_mir_lower_t *lower)
{
    for (anvil_block_t *block = lower->base.func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_PHI)
                break;
            if (!instr->result)
                return false;

            if (ppc_needs_i64_pair(lower, instr->result->type)) {
                anvil_wide_pair_t pair;
                if (!anvil_wide_new(lower->base.mir, anvil_wide_value_unsigned(instr->result), &pair) || !anvil_lower_wide_put(&lower->wide_pairs, instr->result, pair))
                    return false;
                continue;
            }
            anvil_mir_vreg_t vreg = ppc_add_vreg_for_type(lower, instr->result->type);
            if (vreg == ANVIL_MIR_NO_VREG)
                return false;
            if (!anvil_lower_vreg_put(&lower->base.values, instr->result, vreg))
                return false;
        }
    }

    return true;
}

static int64_t ppc_int_constant(anvil_value_t *value)
{
    if (!value)
        return 0;
    if (value->type && !value->type->is_signed)
        return (int64_t)value->data.u;
    return value->data.i;
}

static bool ppc_get_const_int(anvil_value_t *value, int64_t *out)
{
    if (!value || value->kind != ANVIL_VAL_CONST_INT)
        return false;
    if (out)
        *out = ppc_int_constant(value);
    return true;
}

static bool ppc_lower_i64_bitcast_pair(ppc_mir_lower_t *lower, anvil_instr_t *instr);

static bool ppc_mark_helper_call(ppc_mir_lower_t *lower)
{
    anvil_ctx_t *ctx = lower->base.func->parent ? lower->base.func->parent->ctx : NULL;
    return anvil_ctx_get_reloc_model(ctx) == ANVIL_RELOC_STATIC ||
           anvil_mir_set_instr_symbol_got(lower->base.mir, anvil_mir_num_instrs(lower->base.mir) - 1, true);
}

static bool add_return_copy(ppc_mir_lower_t *lower, anvil_value_t *value)
{
    if (!value && lower->sret_in_regs) {
        const ppc_agg_plan_t *plan = &lower->sret_plan;
        anvil_agg_target_t target = ppc_agg_target(lower);
        anvil_mir_vreg_t loaded[8], fixed[8];
        for (unsigned part = 0; part < plan->num_parts; part++) {
            loaded[part] = anvil_agg_load_part(lower->base.mir, &plan->parts[part], lower->sret_object, target);
            if (loaded[part] == ANVIL_MIR_NO_VREG)
                return false;
        }
        for (unsigned part = 0; part < plan->num_parts; part++) {
            const anvil_agg_part_t *p = &plan->parts[part];
            anvil_mir_vreg_t uses[] = {loaded[part]};
            int reg = (p->fp ? lower->desc->fpr_return_reg : lower->desc->gpr_return_reg) + (int)part;
            fixed[part] = anvil_mir_add_vreg_typed(lower->base.mir, p->fp ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR, anvil_agg_part_bits(p, target), false);
            if (fixed[part] == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->base.mir, fixed[part], reg) ||
                !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, fixed[part], uses, 1))
                return false;
        }
        /* RET names the first register; the others stay live up to it. */
        if (plan->num_parts > 1 && !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_KEEPALIVE, ANVIL_MIR_NO_VREG, &fixed[1], plan->num_parts - 1))
            return false;
        return anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, fixed, 1);
    }
    if (!value) {
        return anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, NULL, 0);
    }

    if (ppc_needs_i64_pair(lower, value->type)) {
        anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
        anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
        if (!ppc_ensure_i64_pair(lower, value, &hi, &lo, NULL))
            return false;
        const anvil_mir_vreg_info_t *hi_info = anvil_mir_get_vreg_info(lower->base.mir, hi);
        if (!hi_info)
            return false;
        anvil_mir_vreg_t ret_hi = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 32, hi_info->is_signed);
        anvil_mir_vreg_t ret_lo = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 32, false);
        if (ret_hi == ANVIL_MIR_NO_VREG || ret_lo == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->base.mir, ret_hi, 3) || !anvil_mir_set_fixed_reg(lower->base.mir, ret_lo, 4)) {
            return false;
        }
        anvil_mir_vreg_t hi_uses[] = {hi};
        anvil_mir_vreg_t lo_uses[] = {lo};
        anvil_mir_vreg_t ret_uses[] = {ret_hi};
        return anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, ret_hi, hi_uses, 1) && anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, ret_lo, lo_uses, 1) &&
               anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, ret_uses, 1);
    }

    anvil_mir_vreg_t src = anvil_ir_lower_value(&lower->base, value);
    if (src == ANVIL_MIR_NO_VREG)
        return false;

    /* The ABI has the callee extend narrow integer results. */
    unsigned extend_bits = lower->desc->abi_ops->result_extend_bits;
    const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->base.mir, src);
    bool extend = !ppc_type_is_fp(value->type) && src_info && src_info->reg_class == ANVIL_MIR_REG_GPR && src_info->size_bits < extend_bits;
    if (extend) {
        src = anvil_mir_extend_gpr(lower->base.mir, src, (uint16_t)extend_bits, value->type->is_signed && value->type->kind != ANVIL_TYPE_I1);
        if (src == ANVIL_MIR_NO_VREG)
            return false;
    }

    anvil_mir_vreg_t ret = extend ? anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, (uint16_t)extend_bits, value->type->is_signed)
                                  : ppc_add_vreg_for_type(lower, value->type);
    if (ret == ANVIL_MIR_NO_VREG)
        return false;
    if (!anvil_mir_set_fixed_reg(lower->base.mir, ret, ppc_type_is_fp(value->type) ? lower->desc->fpr_return_reg : lower->desc->gpr_return_reg)) {
        return false;
    }

    anvil_mir_vreg_t uses[] = {src};
    if (!anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, ret, uses, 1)) {
        return false;
    }

    anvil_mir_vreg_t ret_uses[] = {ret};
    return anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, ret_uses, 1);
}

/* Outgoing argument registers: at most every GPR and FPR argument register
 * plus the indirect call target. */
#define PPC_MAX_CALL_USES 24

typedef struct {
    anvil_mir_vreg_t uses[PPC_MAX_CALL_USES];
    size_t num_uses;
    bool fp_in_regs;
} ppc_call_args_t;

/* Places one outgoing value in its argument register or stack slot. */
static bool ppc_outgoing_value(ppc_mir_lower_t *lower, ppc_arg_loc_t loc, anvil_mir_vreg_t value, ppc_call_args_t *args)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(lower->base.mir, value);
    if (!info)
        return false;
    if (loc.reg < 0) {
        anvil_mir_vreg_t uses[] = {value};
        return anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, uses, 1, loc.offset);
    }

    anvil_mir_reg_class_t cls = info->reg_class;
    anvil_mir_vreg_t fixed = anvil_mir_add_vreg_typed(lower->base.mir, cls, info->size_bits, info->is_signed);
    anvil_mir_vreg_t uses[] = {value};
    if (fixed == ANVIL_MIR_NO_VREG || args->num_uses >= PPC_MAX_CALL_USES || !anvil_mir_set_fixed_reg(lower->base.mir, fixed, loc.reg) ||
        !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, fixed, uses, 1))
        return false;
    args->uses[args->num_uses++] = fixed;
    if (cls == ANVIL_MIR_REG_FPR)
        args->fp_in_regs = true;
    return true;
}

/* Passes the object at `src` by value. */
static bool lower_outgoing_aggregate(ppc_mir_lower_t *lower, const ppc_agg_plan_t *plan, anvil_mir_vreg_t src, ppc_arg_state_t *state, ppc_call_args_t *args)
{
    const anvil_ppc_target_desc_t *desc = lower->desc;
    anvil_agg_target_t target = ppc_agg_target(lower);
    size_t first_word = 0;
    if (plan->placement == PPC_AGG_REGS) {
        unsigned in_fprs = ppc_hfa_parts_in_fprs(lower, plan, state);
        anvil_mir_vreg_t values[8];
        for (unsigned part = 0; part < in_fprs; part++) {
            values[part] = anvil_agg_load_part(lower->base.mir, &plan->parts[part], src, target);
            if (values[part] == ANVIL_MIR_NO_VREG)
                return false;
        }
        for (unsigned part = 0; part < in_fprs; part++) {
            ppc_arg_loc_t loc = {desc->fpr_arg_regs[state->fpr++], 0};
            if (!ppc_outgoing_value(lower, loc, values[part], args))
                return false;
        }
        if (in_fprs == plan->num_parts) {
            state->stack += (int64_t)ppc_image_words(lower, plan) * 8;
            return true;
        }
        state->fpr = desc->num_fpr_arg_regs;
        first_word = plan->parts[in_fprs].offset / desc->word_size;
        state->stack += (int64_t)first_word * 8;
    }

    if (plan->align >= 16 && first_word == 0)
        state->stack = (state->stack + 15) & ~INT64_C(15);
    for (size_t index = first_word; index < ppc_image_words(lower, plan); index++) {
        anvil_agg_part_t part = ppc_image_part(lower, plan, index);
        anvil_mir_vreg_t value = anvil_agg_load_part(lower->base.mir, &part, src, target);
        if (value == ANVIL_MIR_NO_VREG || !ppc_outgoing_value(lower, ppc_assign_word(lower, state), value, args))
            return false;
    }
    return true;
}

static bool lower_call(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands == 0)
        return false;
    if (instr->call_cc != ANVIL_CC_SYSV)
        return false;

    anvil_value_t *callee = instr->operands[0];
    anvil_type_t *fn_type = anvil_ir_call_type(callee);
    if (!fn_type)
        return false;

    bool direct_call = anvil_ir_call_is_direct(callee);
    const char *symbol = direct_call ? anvil_ir_call_symbol(callee) : NULL;
    if (direct_call && !symbol)
        return false;

    const anvil_ppc_target_desc_t *desc = lower->desc;
    anvil_agg_target_t target = ppc_agg_target(lower);
    bool variadic = fn_type->data.func.variadic;
    size_t num_named = fn_type->data.func.num_params;
    size_t num_args = instr->num_operands - 1;
    ppc_call_args_t args = {{0}, direct_call ? 0 : 1, false};   /* [0]: target */
    ppc_arg_state_t state = {0, 0, 0};
    bool sret_in_regs = false;
    ppc_agg_plan_t sret_plan;
    anvil_mir_vreg_t sret_object = ANVIL_MIR_NO_VREG;

    for (size_t i = 0; i < num_args; i++) {
        anvil_value_t *arg = instr->operands[i + 1];

        if (ppc_needs_i64_pair(lower, arg->type)) {
            anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG, lo = ANVIL_MIR_NO_VREG;
            int lo_reg = -1;
            if (!desc->abi_ops->assign_pair || !ppc_ensure_i64_pair(lower, arg, &hi, &lo, NULL))
                return false;
            ppc_arg_loc_t loc = desc->abi_ops->assign_pair(desc, &state, &lo_reg);
            ppc_arg_loc_t lo_loc = {lo_reg, loc.offset + 4};
            if (!ppc_outgoing_value(lower, loc, hi, &args) || !ppc_outgoing_value(lower, lo_loc, lo, &args))
                return false;
            continue;
        }

        anvil_mir_vreg_t src = anvil_ir_lower_value(&lower->base, arg);
        if (src == ANVIL_MIR_NO_VREG)
            return false;

        anvil_param_attr_t attr = anvil_call_arg_attr(instr, fn_type, i);
        if (attr != ANVIL_PARAM_PLAIN) {
            ppc_agg_plan_t plan;
            if (!desc->abi_ops->classify_aggregate(arg->type->data.pointee, attr == ANVIL_PARAM_SRET, &plan))
                return false;
            if (attr == ANVIL_PARAM_SRET && plan.placement == PPC_AGG_REGS) {
                /* Stored from the result registers after the call. */
                sret_in_regs = true;
                sret_plan = plan;
                sret_object = src;
                continue;
            }
            /* Unnamed floating aggregates travel like unnamed floating
             * scalars: as their image in the parameter words. */
            if (plan.placement == PPC_AGG_REGS && variadic && i >= num_named && desc->abi_ops->unnamed_fp_in_gprs)
                plan.placement = PPC_AGG_IMAGE;
            if (plan.placement == PPC_AGG_REGS || plan.placement == PPC_AGG_IMAGE) {
                if (!lower_outgoing_aggregate(lower, &plan, src, &state, &args))
                    return false;
                continue;
            }
            if (plan.placement == PPC_AGG_REFERENCE) {
                /* The callee may write its argument: pass a fresh copy. */
                anvil_mir_vreg_t copy = anvil_agg_frame_object(lower->base.mir, plan.size, 16, target);
                if (copy == ANVIL_MIR_NO_VREG || !anvil_agg_copy(lower->base.mir, src, copy, plan.size, target))
                    return false;
                src = copy;
            }
            if (!ppc_outgoing_value(lower, ppc_assign_word(lower, &state), src, &args))
                return false;
            continue;
        }

        if (ppc_type_is_fp(arg->type)) {
            if (variadic && i >= num_named && desc->abi_ops->unnamed_fp_in_gprs) {
                /* The callee reads unnamed arguments from the parameter
                 * save area: pass the double's bits as a general word. */
                if (arg->type->kind == ANVIL_TYPE_F32) {
                    anvil_mir_vreg_t wide = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_FPR, 64, false);
                    anvil_mir_vreg_t uses[] = {src};
                    if (wide == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_FPEXT, wide, uses, 1))
                        return false;
                    src = wide;
                }
                anvil_mir_vreg_t bits = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 64, false);
                anvil_mir_vreg_t uses[] = {src};
                if (bits == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_BITCAST, bits, uses, 1) ||
                    !ppc_outgoing_value(lower, ppc_assign_word(lower, &state), bits, &args))
                    return false;
                continue;
            }
            if (!ppc_outgoing_value(lower, ppc_assign_fpr(lower, &state, arg->type->kind == ANVIL_TYPE_F32), src, &args))
                return false;
            continue;
        }

        /* Integers travel as whole words, extended according to their type. */
        src = anvil_ir_pointer_width(&lower->base, src, ppc_type_is_signed(arg->type));
        if (src == ANVIL_MIR_NO_VREG || !ppc_outgoing_value(lower, ppc_assign_word(lower, &state), src, &args))
            return false;
    }

    /* The target is loaded last, next to the call. */
    if (!direct_call) {
        anvil_mir_vreg_t target_src = anvil_ir_lower_value(&lower->base, callee);
        anvil_mir_vreg_t target_fixed = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, (uint16_t)(desc->word_size * 8), false);
        anvil_mir_vreg_t target_uses[] = {target_src};
        if (target_src == ANVIL_MIR_NO_VREG || target_fixed == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->base.mir, target_fixed, desc->indirect_call_reg) ||
            !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, target_fixed, target_uses, 1))
            return false;
        args.uses[0] = target_fixed;
    }

    bool wide_result = instr->result && ppc_needs_i64_pair(lower, instr->result->type);
    anvil_mir_vreg_t call_def = ANVIL_MIR_NO_VREG;
    if (sret_in_regs) {
        const anvil_agg_part_t *p = &sret_plan.parts[0];
        call_def = anvil_mir_add_vreg_typed(lower->base.mir, p->fp ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR, anvil_agg_part_bits(p, target), false);
        if (call_def == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->base.mir, call_def, p->fp ? desc->fpr_return_reg : desc->gpr_return_reg))
            return false;
    } else if (wide_result) {
        call_def = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 32, instr->result->type->kind != ANVIL_TYPE_U64);
        if (call_def == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->base.mir, call_def, desc->gpr_return_reg))
            return false;
    } else if (instr->result && !ppc_type_is_void(instr->result->type)) {
        call_def = ppc_add_vreg_for_type(lower, instr->result->type);
        if (call_def == ANVIL_MIR_NO_VREG ||
            !anvil_mir_set_fixed_reg(lower->base.mir, call_def, ppc_type_is_fp(instr->result->type) ? desc->fpr_return_reg : desc->gpr_return_reg))
            return false;
    }

    /* For variadic callees the immediate records whether FPRs carry
     * arguments, for the ABI's variadic call setup. */
    if (!anvil_mir_add_call(lower->base.mir, call_def, args.uses, args.num_uses, symbol, instr->call_cc, variadic, args.fp_in_regs ? 1 : 0) ||
        (symbol && !anvil_ir_mark_symbol(&lower->base, callee)))
        return false;

    if (sret_in_regs) {
        anvil_mir_vreg_t results[8] = {call_def};
        for (unsigned part = 1; part < sret_plan.num_parts; part++) {
            const anvil_agg_part_t *p = &sret_plan.parts[part];
            int reg = (p->fp ? desc->fpr_return_reg : desc->gpr_return_reg) + (int)part;
            results[part] = anvil_mir_add_vreg_typed(lower->base.mir, p->fp ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR, anvil_agg_part_bits(p, target), false);
            if (results[part] == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->base.mir, results[part], reg) ||
                !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_CALL_RESULT, results[part], NULL, 0))
                return false;
        }
        for (unsigned part = 0; part < sret_plan.num_parts; part++) {
            const anvil_agg_part_t *p = &sret_plan.parts[part];
            anvil_mir_vreg_t value = anvil_mir_add_vreg_typed(lower->base.mir, p->fp ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR, anvil_agg_part_bits(p, target), false);
            anvil_mir_vreg_t uses[] = {results[part]};
            if (value == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, value, uses, 1) ||
                !anvil_agg_store_part(lower->base.mir, p, value, sret_object, target))
                return false;
        }
        return true;
    }

    if (wide_result) {
        bool is_unsigned = instr->result->type->kind == ANVIL_TYPE_U64;
        anvil_mir_vreg_t ret_lo = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 32, false);
        anvil_mir_vreg_t hi = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 32, !is_unsigned);
        anvil_mir_vreg_t lo = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 32, false);
        anvil_mir_vreg_t hi_uses[] = {call_def};
        anvil_mir_vreg_t lo_uses[] = {ret_lo};
        return ret_lo != ANVIL_MIR_NO_VREG && hi != ANVIL_MIR_NO_VREG && lo != ANVIL_MIR_NO_VREG &&
               anvil_mir_set_fixed_reg(lower->base.mir, ret_lo, desc->gpr_return_reg + 1) &&
               anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_CALL_RESULT, ret_lo, NULL, 0) &&
               anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, hi, hi_uses, 1) &&
               anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, lo, lo_uses, 1) &&
               anvil_lower_pair_put(&lower->wide_pairs, instr->result, hi, lo, is_unsigned);
    }

    if (instr->result && call_def != ANVIL_MIR_NO_VREG) {
        anvil_mir_vreg_t local_result = ppc_add_vreg_for_type(lower, instr->result->type);
        if (local_result == ANVIL_MIR_NO_VREG)
            return false;

        anvil_mir_vreg_t copy_uses[] = {call_def};
        if (!anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, local_result, copy_uses, 1)) {
            return false;
        }
        return anvil_lower_vreg_put(&lower->base.values, instr->result, local_result);
    }

    return true;
}

static bool lower_alloca(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || !instr->result->type || instr->result->type->kind != ANVIL_TYPE_PTR) {
        return false;
    }

    anvil_type_t *element_type = instr->aux_type;
    if (!element_type)
        element_type = instr->result->type->data.pointee;
    if (!element_type)
        return false;

    anvil_mir_vreg_t ptr = ppc_add_vreg_for_type(lower, instr->result->type);
    if (ptr == ANVIL_MIR_NO_VREG)
        return false;

    if (instr->num_operands == 0) {
        int slot = anvil_mir_add_frame_slot(lower->base.mir, ppc_slot_bytes_for_type(lower->desc, element_type), ppc_align_for_type(lower->desc, element_type));
        if (slot < 0)
            return false;
        if (!anvil_mir_add_frame_addr(lower->base.mir, ptr, slot))
            return false;
        return anvil_lower_vreg_put(&lower->base.values, instr->result, ptr);
    }

    if (instr->num_operands != 1)
        return false;
    anvil_mir_vreg_t count = anvil_ir_lower_value(&lower->base, instr->operands[0]);
    if (count == ANVIL_MIR_NO_VREG)
        return false;
    count = anvil_ir_pointer_width(&lower->base, count, false);
    if (count == ANVIL_MIR_NO_VREG)
        return false;

    int64_t elem_size = element_type ? (int64_t)anvil_type_size(element_type) : 1;
    if (elem_size <= 0)
        elem_size = 1;
    anvil_mir_vreg_t uses[] = {count};
    if (!anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_DYN_ALLOCA, ptr, uses, 1, elem_size)) {
        return false;
    }
    return anvil_lower_vreg_put(&lower->base.values, instr->result, ptr);
}

static bool ppc_ensure_i64_pair(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *hi, anvil_mir_vreg_t *lo, bool *is_unsigned);

static bool lower_cast(ppc_mir_lower_t *lower, anvil_instr_t *instr, anvil_mir_opcode_t mir_op)
{
    if (instr->num_operands != 1 || !instr->result)
        return false;
    /* A pointer reinterpreted as another pointer is the same value (a
     * constant offset from a base stays one, for the accesses to fold). */
    if (anvil_cast_keeps_value(instr)) {
        anvil_mir_vreg_t offset_base = ANVIL_MIR_NO_VREG;
        int64_t offset = 0;
        if (anvil_lower_address_get(&lower->base.addresses, instr->operands[0], &offset_base, &offset))
            return anvil_lower_address_put(&lower->base.addresses, instr->result, offset_base, offset);
        anvil_mir_vreg_t same = anvil_ir_lower_value(&lower->base, instr->operands[0]);
        return same != ANVIL_MIR_NO_VREG && anvil_lower_vreg_put(&lower->base.values, instr->result, same);
    }
    bool dst_pair = ppc_needs_i64_pair(lower, instr->result->type);
    bool src_pair = ppc_needs_i64_pair(lower, instr->operands[0]->type);
    anvil_wide_target_t target = ppc_wide_target(lower);
    if (dst_pair && src_pair) {
        if (mir_op != ANVIL_MIR_OP_BITCAST)
            return false;
        return ppc_lower_i64_bitcast_pair(lower, instr);
    }
    if (dst_pair) {
        /* To 64 bits: extension (pointers zero-extend) or FP conversion. */
        anvil_mir_vreg_t src = anvil_ir_lower_value(&lower->base, instr->operands[0]);
        anvil_wide_pair_t out;
        bool ok = src != ANVIL_MIR_NO_VREG &&
                  (mir_op == ANVIL_MIR_OP_FPTOSI || mir_op == ANVIL_MIR_OP_FPTOUI
                       ? anvil_wide_from_fp(lower->base.mir, &target, src, mir_op == ANVIL_MIR_OP_FPTOUI, &out)
                       : anvil_wide_extend(lower->base.mir, src, mir_op == ANVIL_MIR_OP_SEXT, anvil_wide_value_unsigned(instr->result), &out));
        return ok && anvil_lower_wide_put(&lower->wide_pairs, instr->result, out);
    }
    if (src_pair) {
        /* From 64 bits: the low word (truncation, pointers) or FP. */
        anvil_wide_pair_t a;
        anvil_mir_vreg_t result;
        if (!ppc_ensure_wide(lower, instr->operands[0], &a))
            return false;
        if (mir_op == ANVIL_MIR_OP_SITOFP || mir_op == ANVIL_MIR_OP_UITOFP) {
            if (!anvil_wide_to_fp(lower->base.mir, &target, a, mir_op == ANVIL_MIR_OP_UITOFP, ppc_bits_for_type(lower->desc, instr->result->type), &result))
                return false;
        } else {
            result = anvil_wide_truncate(lower->base.mir, a, ppc_bits_for_type(lower->desc, instr->result->type), ppc_type_is_signed(instr->result->type));
        }
        return result != ANVIL_MIR_NO_VREG && anvil_lower_vreg_put(&lower->base.values, instr->result, result);
    }

    anvil_mir_vreg_t src = anvil_ir_lower_value(&lower->base, instr->operands[0]);
    anvil_mir_vreg_t def = ppc_add_vreg_for_type(lower, instr->result->type);
    if (src == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG)
        return false;
    /* Narrow integers convert from their extended value. */
    src = anvil_mir_int_to_fp_source(lower->base.mir, mir_op, src, 32);
    if (src == ANVIL_MIR_NO_VREG)
        return false;

    if (instr->result->type->kind == ANVIL_TYPE_I1 && instr->operands[0]->type->kind != ANVIL_TYPE_I1) {
        const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->base.mir, src);
        if (src_info && src_info->reg_class == ANVIL_MIR_REG_FPR) {
            if (mir_op != ANVIL_MIR_OP_FPTOUI)
                return false;
            anvil_mir_vreg_t converted = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 32, false);
            anvil_mir_vreg_t convert_use[] = {src};
            if (converted == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(lower->base.mir, mir_op, converted, convert_use, 1))
                return false;
            src = converted;
            src_info = anvil_mir_get_vreg_info(lower->base.mir, src);
        }
        anvil_mir_vreg_t narrowed = src;
        if (!src_info || src_info->reg_class != ANVIL_MIR_REG_GPR)
            return false;
        if (src_info->size_bits > 8) {
            narrowed = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 8, false);
            anvil_mir_vreg_t narrow_use[] = {src};
            if (narrowed == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_TRUNC, narrowed, narrow_use, 1))
                return false;
        }
        anvil_mir_vreg_t one = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 8, false);
        anvil_mir_vreg_t uses[] = {narrowed, one};
        if (one == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_MOV, one, 1) || !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_AND, def, uses, 2))
            return false;
        return anvil_lower_vreg_put(&lower->base.values, instr->result, def);
    }

    if (mir_op == ANVIL_MIR_OP_BITCAST) {
        const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->base.mir, src);
        const anvil_mir_vreg_info_t *dst_info = anvil_mir_get_vreg_info(lower->base.mir, def);
        if (!src_info || !dst_info)
            return false;
        if (src_info->size_bits != dst_info->size_bits) {
            if (instr->op != ANVIL_OP_PTRTOINT && instr->op != ANVIL_OP_INTTOPTR)
                return false;
            mir_op = src_info->size_bits < dst_info->size_bits ? ANVIL_MIR_OP_ZEXT : ANVIL_MIR_OP_TRUNC;
        }
    }

    anvil_mir_vreg_t uses[] = {src};
    if (!anvil_mir_add_instr(lower->base.mir, mir_op, def, uses, 1))
        return false;
    return anvil_lower_vreg_put(&lower->base.values, instr->result, def);
}

static bool lower_select(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands != 3 || !instr->result)
        return false;
    if (ppc_needs_i64_pair(lower, instr->result->type)) {
        /* One select per word. */
        anvil_wide_pair_t then_pair, else_pair, out;
        anvil_mir_vreg_t cond = anvil_ir_lower_value(&lower->base, instr->operands[0]);
        if (cond == ANVIL_MIR_NO_VREG || !ppc_ensure_wide(lower, instr->operands[1], &then_pair) ||
            !ppc_ensure_wide(lower, instr->operands[2], &else_pair) ||
            !anvil_wide_new(lower->base.mir, anvil_wide_value_unsigned(instr->result), &out))
            return false;
        anvil_mir_vreg_t hi_uses[] = {cond, then_pair.hi, else_pair.hi};
        anvil_mir_vreg_t lo_uses[] = {cond, then_pair.lo, else_pair.lo};
        return anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_SELECT, out.hi, hi_uses, 3) &&
               anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_SELECT, out.lo, lo_uses, 3) &&
               anvil_lower_wide_put(&lower->wide_pairs, instr->result, out);
    }
    anvil_mir_vreg_t cond = anvil_ir_lower_value(&lower->base, instr->operands[0]);
    anvil_mir_vreg_t then_v = anvil_ir_lower_value(&lower->base, instr->operands[1]);
    anvil_mir_vreg_t else_v = anvil_ir_lower_value(&lower->base, instr->operands[2]);
    anvil_mir_vreg_t def = ppc_add_vreg_for_type(lower, instr->result->type);
    if (cond == ANVIL_MIR_NO_VREG || then_v == ANVIL_MIR_NO_VREG || else_v == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) {
        return false;
    }

    anvil_mir_vreg_t uses[] = {cond, then_v, else_v};
    if (!anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_SELECT, def, uses, 3)) {
        return false;
    }
    return anvil_lower_vreg_put(&lower->base.values, instr->result, def);
}

static bool ppc_add_memory_instr(ppc_mir_lower_t *lower, anvil_mir_opcode_t op, anvil_mir_vreg_t def, anvil_mir_vreg_t *uses, size_t num_uses, int64_t offset)
{
    if (offset == 0) {
        return anvil_mir_add_instr(lower->base.mir, op, def, uses, num_uses);
    }
    return anvil_mir_add_instr_imm_uses(lower->base.mir, op, def, uses, num_uses, offset);
}

static bool ppc_get_wide_const_value(ppc_mir_lower_t *lower, anvil_value_t *value, int64_t *out)
{
    if (ppc_get_const_int(value, out))
        return true;
    return anvil_lower_constant_get(&lower->wide_consts, value, out);
}

static anvil_mir_vreg_t ppc_add_i32_vreg(ppc_mir_lower_t *lower, bool is_signed)
{
    return anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 32, is_signed);
}

static anvil_mir_vreg_t ppc_add_bool_vreg(ppc_mir_lower_t *lower)
{
    return anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 8, false);
}

static bool ppc_ensure_i64_pair(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *hi, anvil_mir_vreg_t *lo, bool *is_unsigned)
{
    int64_t imm = 0;
    if (value && ppc_get_wide_const_value(lower, value, &imm)) {
        /* Materialized at each use: a constant may be used in blocks the
         * first use does not dominate. */
        bool value_unsigned = value->type && value->type->kind == ANVIL_TYPE_U64;
        anvil_wide_pair_t pair;
        if (!anvil_wide_constant(lower->base.mir, imm, value_unsigned, &pair))
            return false;
        *hi = pair.hi;
        *lo = pair.lo;
        if (is_unsigned)
            *is_unsigned = value_unsigned;
        return true;
    }
    return anvil_lower_pair_get(&lower->wide_pairs, value, hi, lo, is_unsigned);
}

static bool ppc_ensure_wide(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_wide_pair_t *pair)
{
    return ppc_ensure_i64_pair(lower, value, &pair->hi, &pair->lo, NULL);
}

/* Run-time helpers for 64-bit arithmetic, called with the C convention:
 * arguments where the ABI module assigns them, a 64-bit result in r3:r4 and
 * a floating one in f1. */
static bool ppc_wide_call_helper(void *ctx, const char *name, const anvil_wide_pair_t *args, size_t num_args,
                                 anvil_wide_pair_t result)
{
    ppc_mir_lower_t *lower = ctx;
    const anvil_ppc_target_desc_t *desc = lower->desc;
    ppc_arg_state_t state = {0, 0, 0};
    anvil_mir_vreg_t uses[8];
    size_t num_uses = 0;
    for (size_t i = 0; i < num_args; i++) {
        anvil_mir_vreg_t values[2] = {args[i].hi, args[i].lo};
        ppc_arg_loc_t locs[2];
        size_t count = 1;
        const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(lower->base.mir, args[i].lo);
        if (!info)
            return false;
        if (args[i].hi != ANVIL_MIR_NO_VREG) {
            int lo_reg = -1;
            if (!desc->abi_ops->assign_pair)
                return false;
            locs[0] = desc->abi_ops->assign_pair(desc, &state, &lo_reg);
            locs[1] = (ppc_arg_loc_t){lo_reg, locs[0].offset + 4};
            count = 2;
        } else {
            values[0] = args[i].lo;
            locs[0] = info->reg_class == ANVIL_MIR_REG_FPR ? desc->abi_ops->assign_fpr(desc, &state, info->size_bits == 32)
                                                           : desc->abi_ops->assign_word(desc, &state);
        }
        for (size_t w = 0; w < count; w++) {
            const anvil_mir_vreg_info_t *winfo = anvil_mir_get_vreg_info(lower->base.mir, values[w]);
            if (!winfo || locs[w].reg < 0 || num_uses >= 8)
                return false;
            anvil_mir_vreg_t fixed = anvil_mir_add_vreg_typed(lower->base.mir, winfo->reg_class, winfo->size_bits, winfo->is_signed);
            anvil_mir_vreg_t copy_uses[] = {values[w]};
            if (fixed == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->base.mir, fixed, locs[w].reg) ||
                !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, fixed, copy_uses, 1))
                return false;
            uses[num_uses++] = fixed;
        }
    }

    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(lower->base.mir, result.lo);
    if (!info)
        return false;
    bool fp = info->reg_class == ANVIL_MIR_REG_FPR;
    anvil_mir_vreg_t first = result.hi != ANVIL_MIR_NO_VREG ? result.hi : result.lo;
    const anvil_mir_vreg_info_t *first_info = anvil_mir_get_vreg_info(lower->base.mir, first);
    anvil_mir_vreg_t ret_first = anvil_mir_add_vreg_typed(lower->base.mir, first_info->reg_class, first_info->size_bits, first_info->is_signed);
    if (ret_first == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->base.mir, ret_first, fp ? desc->fpr_return_reg : desc->gpr_return_reg) ||
        !anvil_mir_add_call(lower->base.mir, ret_first, uses, num_uses, name, ANVIL_CC_SYSV, false, 0) || !ppc_mark_helper_call(lower))
        return false;
    anvil_mir_vreg_t ret_lo = ANVIL_MIR_NO_VREG;
    if (result.hi != ANVIL_MIR_NO_VREG) {
        ret_lo = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 32, false);
        if (ret_lo == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->base.mir, ret_lo, desc->gpr_return_reg + 1) ||
            !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_CALL_RESULT, ret_lo, NULL, 0))
            return false;
    }
    anvil_mir_vreg_t first_uses[] = {ret_first};
    if (!anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, first, first_uses, 1))
        return false;
    anvil_mir_vreg_t lo_uses[] = {ret_lo};
    return result.hi == ANVIL_MIR_NO_VREG || anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, result.lo, lo_uses, 1);
}

static anvil_wide_target_t ppc_wide_target(ppc_mir_lower_t *lower)
{
    return (anvil_wide_target_t){ppc_wide_call_helper, lower};
}

static bool ppc_lower_load_i64_pair(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || instr->num_operands != 1)
        return false;

    anvil_mir_vreg_t base = ANVIL_MIR_NO_VREG;
    int64_t offset = 0;
    if (!anvil_ir_lower_memory_address(&lower->base, instr->operands[0], &base, &offset)) {
        return false;
    }
    if (offset > INT64_MAX - 4)
        return false;

    bool is_unsigned = instr->result->type && instr->result->type->kind == ANVIL_TYPE_U64;
    anvil_mir_vreg_t hi = ppc_add_i32_vreg(lower, !is_unsigned);
    anvil_mir_vreg_t lo = ppc_add_i32_vreg(lower, false);
    if (hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG)
        return false;

    anvil_mir_vreg_t hi_uses[] = {base};
    if (!ppc_add_memory_instr(lower, ANVIL_MIR_OP_LOAD, hi, hi_uses, 1, offset)) {
        return false;
    }

    anvil_mir_vreg_t lo_uses[] = {base};
    if (!ppc_add_memory_instr(lower, ANVIL_MIR_OP_LOAD, lo, lo_uses, 1, offset + 4)) {
        return false;
    }

    return anvil_lower_pair_put(&lower->wide_pairs, instr->result, hi, lo, is_unsigned);
}

static bool ppc_lower_store_i64_const(ppc_mir_lower_t *lower, int64_t imm, anvil_mir_vreg_t base, int64_t offset)
{
    if (offset > INT64_MAX - 4)
        return false;

    uint64_t bits = (uint64_t)imm;
    int32_t hi_imm = (int32_t)(bits >> 32);
    int32_t lo_imm = (int32_t)(bits & 0xffffffffu);

    anvil_mir_vreg_t hi = ppc_add_i32_vreg(lower, true);
    anvil_mir_vreg_t lo = ppc_add_i32_vreg(lower, false);
    if (hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG)
        return false;

    if (!anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_MOV, hi, (int64_t)hi_imm)) {
        return false;
    }
    anvil_mir_vreg_t hi_uses[] = {hi, base};
    if (!ppc_add_memory_instr(lower, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, hi_uses, 2, offset)) {
        return false;
    }

    if (!anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_MOV, lo, (int64_t)lo_imm)) {
        return false;
    }
    anvil_mir_vreg_t lo_uses[] = {lo, base};
    return ppc_add_memory_instr(lower, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, lo_uses, 2, offset + 4);
}

static bool ppc_lower_store_i64_pair(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t base, int64_t offset)
{
    if (offset > INT64_MAX - 4)
        return false;

    anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
    if (!ppc_ensure_i64_pair(lower, value, &hi, &lo, NULL))
        return false;

    anvil_mir_vreg_t hi_uses[] = {hi, base};
    if (!ppc_add_memory_instr(lower, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, hi_uses, 2, offset)) {
        return false;
    }

    anvil_mir_vreg_t lo_uses[] = {lo, base};
    return ppc_add_memory_instr(lower, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, lo_uses, 2, offset + 4);
}

static bool ppc_lower_i64_bitcast_pair(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || instr->num_operands != 1)
        return false;

    anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
    if (!ppc_ensure_i64_pair(lower, instr->operands[0], &hi, &lo, NULL)) {
        return false;
    }

    int64_t imm = 0;
    if (ppc_get_wide_const_value(lower, instr->operands[0], &imm) && !anvil_lower_constant_put(&lower->wide_consts, instr->result, imm)) {
        return false;
    }

    bool is_unsigned = instr->result->type && instr->result->type->kind == ANVIL_TYPE_U64;
    return anvil_lower_pair_put(&lower->wide_pairs, instr->result, hi, lo, is_unsigned);
}

/* 64-bit binary operation through the shared wide-integer legalizer. */
static bool ppc_lower_wide_binary(ppc_mir_lower_t *lower, anvil_instr_t *instr, anvil_mir_opcode_t mir_op)
{
    if (!instr->result)
        return false;
    anvil_wide_pair_t a, b, out;
    bool shift = instr->op == ANVIL_OP_SHL || instr->op == ANVIL_OP_SHR || instr->op == ANVIL_OP_SAR;
    if (!ppc_ensure_wide(lower, instr->operands[0], &a))
        return false;
    if (shift && !ppc_needs_i64_pair(lower, instr->operands[1]->type)) {
        /* A shift count may be narrower than the shifted value. */
        b.hi = ANVIL_MIR_NO_VREG;
        b.lo = anvil_ir_pointer_width(&lower->base, anvil_ir_lower_value(&lower->base, instr->operands[1]), false);
        if (b.lo == ANVIL_MIR_NO_VREG)
            return false;
    } else if (!ppc_ensure_wide(lower, instr->operands[1], &b)) {
        return false;
    }
    if (anvil_ir_compare_op(mir_op)) {
        anvil_mir_vreg_t result = ppc_add_bool_vreg(lower);
        return result != ANVIL_MIR_NO_VREG && anvil_wide_compare(lower->base.mir, mir_op, a, b, result) &&
               anvil_lower_vreg_put(&lower->base.values, instr->result, result);
    }

    anvil_wide_target_t target = ppc_wide_target(lower);
    bool is_unsigned = anvil_wide_value_unsigned(instr->result);
    bool ok;
    if (shift) {
        int64_t count = 0;
        bool known = ppc_get_const_int(instr->operands[1], &count);
        ok = anvil_wide_shift(lower->base.mir, &target, instr->op, a, b.lo, known, count, is_unsigned, &out);
    } else {
        ok = anvil_wide_binary(lower->base.mir, &target, instr->op, a, b, is_unsigned, &out);
    }
    return ok && anvil_lower_wide_put(&lower->wide_pairs, instr->result, out);
}

static bool lower_instr(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->op == ANVIL_OP_NOP)
        return true;
    if (instr->op == ANVIL_OP_PHI)
        return true;

    /* Vector arithmetic and broadcasts, and the scalar operations the
     * target's table prices (FMA, FSQRT), where the target has them. */
    anvil_vector_mir_form_t vector;
    if (anvil_vector_mir_form(lower->base.func->owner_ctx, instr, &vector)) {
        anvil_mir_vreg_t uses[ANVIL_VECTOR_MAX_USES] = {ANVIL_MIR_NO_VREG, ANVIL_MIR_NO_VREG, ANVIL_MIR_NO_VREG};
        for (size_t operand = 0; operand < vector.num_uses; operand++)
            uses[operand] = anvil_ir_lower_value(&lower->base, instr->operands[operand]);
        anvil_mir_vreg_t def = vector.supported ? ppc_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
        return anvil_vector_mir_emit(lower->base.mir, &vector, def, uses) && anvil_lower_vreg_put(&lower->base.values, instr->result, def);
    }

    anvil_mir_opcode_t mir_op;
    if (instr->num_operands == 2 && anvil_ir_binary_op(instr->op, &mir_op)) {
        if (ppc_needs_i64_pair(lower, instr->operands[0]->type) || (instr->result && ppc_needs_i64_pair(lower, instr->result->type)))
            return ppc_lower_wide_binary(lower, instr, mir_op);

        anvil_mir_vreg_t lhs = anvil_ir_lower_value(&lower->base, instr->operands[0]);
        anvil_mir_vreg_t rhs = anvil_ir_lower_value(&lower->base, instr->operands[1]);
        anvil_mir_vreg_t def = instr->result ? ppc_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
        if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) {
            return false;
        }
        if (!anvil_ir_compare_op(mir_op) && !anvil_ir_match_operand_sizes(&lower->base, &lhs, &rhs, def)) {
            return false;
        }

        anvil_mir_vreg_t uses[] = {lhs, rhs};
        bool added = mir_op == ANVIL_MIR_OP_FCMP ? anvil_mir_add_instr_imm_uses(lower->base.mir, mir_op, def, uses, 2, instr->fcmp_pred)
                                                 : anvil_mir_add_binary_extending(lower->base.mir, mir_op, def, lhs, rhs, 32);
        if (!added) {
            return false;
        }
        return anvil_lower_vreg_put(&lower->base.values, instr->result, def);
    }

    if (instr->num_operands == 1 && anvil_ir_unary_op(instr->op, &mir_op)) {
        if (instr->result && ppc_needs_i64_pair(lower, instr->result->type)) {
            anvil_wide_pair_t a, out;
            bool is_unsigned = anvil_wide_value_unsigned(instr->result);
            if (!ppc_ensure_wide(lower, instr->operands[0], &a))
                return false;
            bool ok = instr->op == ANVIL_OP_NEG ? anvil_wide_neg(lower->base.mir, a, is_unsigned, &out)
                    : instr->op == ANVIL_OP_NOT ? anvil_wide_not(lower->base.mir, a, is_unsigned, &out) : false;
            return ok && anvil_lower_wide_put(&lower->wide_pairs, instr->result, out);
        }

        anvil_mir_vreg_t src = anvil_ir_lower_value(&lower->base, instr->operands[0]);
        anvil_mir_vreg_t def = instr->result ? ppc_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
        if (src == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG)
            return false;

        anvil_mir_vreg_t uses[] = {src};
        if (!anvil_mir_add_instr(lower->base.mir, mir_op, def, uses, 1))
            return false;
        return anvil_lower_vreg_put(&lower->base.values, instr->result, def);
    }

    if (instr->num_operands == 1 && anvil_ir_cast_op(instr->op, &mir_op)) {
        return lower_cast(lower, instr, mir_op);
    }

    switch (instr->op) {
    case ANVIL_OP_ATOMIC_LOAD:
    case ANVIL_OP_ATOMIC_STORE:
    case ANVIL_OP_ATOMIC_RMW:
    case ANVIL_OP_ATOMIC_CMPXCHG:
    case ANVIL_OP_ATOMIC_FENCE: {
        anvil_mir_vreg_t uses[3];
        for (size_t operand = 0; operand < instr->num_operands && operand < 3; operand++) {
            if (ppc_needs_i64_pair(lower, instr->operands[operand]->type)) {
                if (lower->base.func->parent && lower->base.func->parent->ctx)
                    anvil_set_error(lower->base.func->parent->ctx, ANVIL_ERR_CODEGEN, "32-bit PowerPC atomics on 64-bit values are not supported");
                return false;
            }
            uses[operand] = anvil_ir_lower_value(&lower->base, instr->operands[operand]);
            if (uses[operand] == ANVIL_MIR_NO_VREG)
                return false;
        }

        anvil_mir_vreg_t def = instr->result ? ppc_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
        if ((instr->result && def == ANVIL_MIR_NO_VREG) || instr->num_operands > 3 ||
            !anvil_mir_add_atomic(lower->base.mir, instr->op, def, uses, instr->num_operands, &instr->atomic))
            return false;

        /* r0 and r5-r10 are the reservation loop's temporaries; r3, r4 and
         * r11 stage the operands (one parallel move, reading them first). */
        uint64_t temporaries = UINT64_C(1) | (UINT64_C(0xff) << 3) | (UINT64_C(1) << 11);
        if (!anvil_mir_set_instr_clobbers(lower->base.mir, anvil_mir_num_instrs(lower->base.mir) - 1, ANVIL_MIR_REG_GPR, temporaries))
            return false;
        return !instr->result || anvil_lower_vreg_put(&lower->base.values, instr->result, def);
    }
    case ANVIL_OP_VA_START: {
        if (!instr->result || !lower->base.func->type->data.func.variadic || !lower->desc->abi_ops->add_va_start)
            return false;
        anvil_mir_vreg_t def = ppc_add_vreg_for_type(lower, instr->result->type);
        return def != ANVIL_MIR_NO_VREG && lower->desc->abi_ops->add_va_start(lower->base.mir, def, &lower->named_args) &&
               anvil_lower_vreg_put(&lower->base.values, instr->result, def);
    }
    case ANVIL_OP_ALLOCA:
        return lower_alloca(lower, instr);
    case ANVIL_OP_GEP:
        return anvil_ir_lower_gep(&lower->base, instr);
    case ANVIL_OP_STRUCT_GEP:
        return anvil_ir_lower_struct_gep(&lower->base, instr);
    case ANVIL_OP_SELECT:
        return lower_select(lower, instr);
    case ANVIL_OP_LOAD: {
        if (instr->num_operands != 1 || !instr->result)
            return false;
        if (ppc_needs_i64_pair(lower, instr->result->type)) {
            return ppc_lower_load_i64_pair(lower, instr);
        }

        anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
        int64_t offset = 0;
        if (!anvil_ir_lower_memory_address(&lower->base, instr->operands[0], &ptr, &offset)) {
            return false;
        }
        anvil_mir_vreg_t def = ppc_add_vreg_for_type(lower, instr->result->type);
        if (def == ANVIL_MIR_NO_VREG)
            return false;
        /* Its sign unobserved: loaded zero-extended. */
        if (instr->sign_unobserved && !anvil_mir_set_vreg_signed(lower->base.mir, def, false))
            return false;
        bool is_i1 = instr->result->type->kind == ANVIL_TYPE_I1;
        anvil_mir_vreg_t loaded = is_i1 ? anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 8, false) : def;
        if (loaded == ANVIL_MIR_NO_VREG)
            return false;
        anvil_mir_vreg_t uses[] = {ptr};
        bool ok = offset == 0 ? anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_LOAD, loaded, uses, 1) : anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_LOAD, loaded, uses, 1, offset);
        if (!ok)
            return false;
        if (is_i1) {
            anvil_mir_vreg_t one = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 8, false);
            anvil_mir_vreg_t norm_uses[] = {loaded, one};
            if (one == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_MOV, one, 1) || !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_AND, def, norm_uses, 2))
                return false;
        }
        return anvil_lower_vreg_put(&lower->base.values, instr->result, def);
    }
    case ANVIL_OP_STORE: {
        if (instr->num_operands != 2)
            return false;
        if (ppc_needs_i64_pair(lower, instr->operands[0]->type)) {
            anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
            int64_t offset = 0;
            if (!anvil_ir_lower_memory_address(&lower->base, instr->operands[1], &ptr, &offset)) {
                return false;
            }

            int64_t imm = 0;
            if (ppc_get_wide_const_value(lower, instr->operands[0], &imm)) {
                return ppc_lower_store_i64_const(lower, imm, ptr, offset);
            }
            if (anvil_lower_pair_get(&lower->wide_pairs, instr->operands[0], NULL, NULL, NULL)) {
                return ppc_lower_store_i64_pair(lower, instr->operands[0], ptr, offset);
            }
        }

        anvil_mir_vreg_t val = anvil_ir_lower_value(&lower->base, instr->operands[0]);
        anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
        int64_t offset = 0;
        if (!anvil_ir_lower_memory_address(&lower->base, instr->operands[1], &ptr, &offset)) {
            return false;
        }
        if (val == ANVIL_MIR_NO_VREG || ptr == ANVIL_MIR_NO_VREG) {
            return false;
        }
        if (instr->operands[0]->type->kind == ANVIL_TYPE_I1) {
            anvil_mir_vreg_t one = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 8, false);
            anvil_mir_vreg_t normalized = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 8, false);
            anvil_mir_vreg_t norm_uses[] = {val, one};
            if (one == ANVIL_MIR_NO_VREG || normalized == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_MOV, one, 1) ||
                !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_AND, normalized, norm_uses, 2))
                return false;
            val = normalized;
        }
        anvil_mir_vreg_t uses[] = {val, ptr};
        if (offset == 0) {
            return anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, uses, 2);
        }
        return anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, uses, 2, offset);
    }
    case ANVIL_OP_CALL:
        return lower_call(lower, instr);
    case ANVIL_OP_BR:
        return anvil_ir_lower_branch(&lower->base, instr);
    case ANVIL_OP_BR_COND:
        return anvil_ir_lower_cond_branch(&lower->base, instr);
    case ANVIL_OP_SWITCH:
        return anvil_ir_lower_switch(&lower->base, instr);
    case ANVIL_OP_RET:
        if (instr->num_operands == 0)
            return add_return_copy(lower, NULL);
        if (instr->num_operands == 1) {
            return add_return_copy(lower, instr->operands[0]);
        }
        return false;
    default:
        return false;
    }
}

/* anvil_ir_lower_t hooks: a 64-bit integer in a word pair. */
static anvil_mir_vreg_t ir_vreg_for_type(anvil_ir_lower_t *base, anvil_type_t *type)
{
    return ppc_add_vreg_for_type((ppc_mir_lower_t *)base, type);
}

static bool ir_value_regs(anvil_ir_lower_t *base, anvil_value_t *value, anvil_mir_vreg_t regs[2], size_t *count)
{
    ppc_mir_lower_t *lower = (ppc_mir_lower_t *)base;
    if (ppc_needs_i64_pair(lower, value->type)) {
        anvil_wide_pair_t pair;
        regs[0] = regs[1] = ANVIL_MIR_NO_VREG;
        *count = 2;
        if (!ppc_ensure_wide(lower, value, &pair))
            return false;
        regs[0] = pair.hi;
        regs[1] = pair.lo;
        return true;
    }
    regs[0] = anvil_ir_lower_value(&lower->base, value);
    *count = 1;
    return regs[0] != ANVIL_MIR_NO_VREG;
}

anvil_mir_func_t *anvil_ppc_lower_func_to_mir(anvil_func_t *func, anvil_ppc_variant_t variant)
{
    const anvil_ppc_target_desc_t *desc = anvil_ppc_get_target_desc(variant);
    if (!desc || !func || func->is_declaration || !func->type || func->type->kind != ANVIL_TYPE_FUNC || func->type->data.func.cc != ANVIL_CC_SYSV)
        return NULL;

    ppc_mir_lower_t lower;
    memset(&lower, 0, sizeof(lower));
    lower.desc = desc;
    lower.base.func = func;
    lower.base.value = anvil_ir_lower_value;
    lower.base.vreg_for_type = ir_vreg_for_type;
    lower.base.pointer_bits = (unsigned)(desc->word_size * 8);
    lower.base.value_regs = ir_value_regs;
    lower.base.mir = anvil_mir_func_create(func->name);
    anvil_mir_func_set_linkage(lower.base.mir, func->linkage);
    anvil_mir_func_set_cpu_features(lower.base.mir, anvil_ctx_get_cpu_features(func->owner_ctx));
    if (!lower.base.mir)
        return NULL;

    anvil_opt_cfg_t cfg;
    if (!anvil_opt_cfg_build(func, &cfg)) {
        anvil_mir_func_destroy(lower.base.mir);
        return NULL;
    }

    if (!anvil_opt_cfg_build_layout(&cfg) || !anvil_lower_create_blocks(lower.base.mir, lower.base.func, &lower.base.blocks) || !lower_params(&lower) || !prepare_phi_results(&lower)) {
        anvil_opt_cfg_destroy(&cfg);
        anvil_mir_func_destroy(lower.base.mir);
        ppc_lower_free(&lower);
        return NULL;
    }

    for (size_t rank = 0; rank < cfg.count; rank++) {
        anvil_block_t *block = cfg.blocks[cfg.layout[rank]];
        anvil_mir_block_t mir_block = anvil_lower_block_get(&lower.base.blocks, block);
        if (mir_block == ANVIL_MIR_NO_BLOCK || !anvil_mir_set_current_block(lower.base.mir, mir_block)) {
            anvil_opt_cfg_destroy(&cfg);
            anvil_mir_func_destroy(lower.base.mir);
            ppc_lower_free(&lower);
            return NULL;
        }

        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            size_t first = anvil_mir_num_instrs(lower.base.mir);
            if (!lower_instr(&lower, instr) || !anvil_mir_annotate_memory(lower.base.mir, first, &instr->memory_access) ||
                (instr->op == ANVIL_OP_CALL && !anvil_mir_annotate_call_effects(lower.base.mir, first, anvil_instr_call_effects(instr)))) {
                anvil_opt_cfg_destroy(&cfg);
                anvil_mir_func_destroy(lower.base.mir);
                ppc_lower_free(&lower);
                return NULL;
            }
        }
    }

    ppc_lower_free(&lower);
    anvil_opt_cfg_destroy(&cfg);
    return lower.base.mir;
}
