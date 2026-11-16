#include "../common/lower_map.h"
#include "../common/switch_lower.h"
#include "ppc_internal.h"
#include "../common/narrow_mir.h"
#include "../common/wide_mir.h"

typedef struct {
    anvil_ir_lower_t base;
    const anvil_ppc_target_desc_t *desc;
    /* SRET result returned in registers: RET loads it from this object. */
    bool sret_in_regs;
    ppc_agg_plan_t sret_plan;
    anvil_mir_vreg_t sret_object;
    /* Argument state after the named parameters (variadic callees). */
    ppc_arg_state_t named_args;
} ppc_mir_lower_t;

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

static bool ppc_needs_i64_pair(const ppc_mir_lower_t *lower, anvil_type_t *type)
{
    return lower && lower->desc && lower->desc->word_size == 4 && ppc_type_is_64bit_integer(type);
}

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
            if (hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG || !anvil_lower_pair_put(&lower->base.pairs, param, hi, lo, is_unsigned))
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
        anvil_wide_pair_t pair;
        if (!anvil_ir_value_pair(&lower->base, value, &pair))
            return false;
        anvil_mir_vreg_t hi = pair.hi, lo = pair.lo;
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
            anvil_wide_pair_t pair;
            int lo_reg = -1;
            if (!desc->abi_ops->assign_pair || !anvil_ir_value_pair(&lower->base, arg, &pair))
                return false;
            anvil_mir_vreg_t hi = pair.hi, lo = pair.lo;
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
               anvil_lower_pair_put(&lower->base.pairs, instr->result, hi, lo, is_unsigned);
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

static bool target_instr(anvil_ir_lower_t *base, anvil_instr_t *instr)
{
    ppc_mir_lower_t *lower = (ppc_mir_lower_t *)base;
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
    case ANVIL_OP_CALL:
        return lower_call(lower, instr);
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

static bool ir_lower_params(anvil_ir_lower_t *base)
{
    return lower_params((ppc_mir_lower_t *)base);
}

static bool ir_pair_type(const anvil_ir_lower_t *base, anvil_type_t *type)
{
    return ppc_needs_i64_pair((const ppc_mir_lower_t *)base, type);
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
    lower.base.vreg_for_type = ir_vreg_for_type;
    lower.base.lower_params = ir_lower_params;
    lower.base.target_instr = target_instr;
    lower.base.narrowest_operation_bits = 32;
    lower.base.pointer_bits = (unsigned)(desc->word_size * 8);
    lower.base.stack_alignment = desc->stack_alignment;
    lower.base.pair_type = ir_pair_type;
    lower.base.wide = (anvil_wide_target_t){ppc_wide_call_helper, &lower, lower.desc->abi_ops->runtime_helpers};
    lower.base.select_classes = ANVIL_IR_SELECT_EVERY_CLASS;
    return anvil_ir_lower_function(&lower.base);
}
