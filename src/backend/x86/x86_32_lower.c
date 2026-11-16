#include "../common/lower_map.h"
#include "../common/switch_lower.h"
#include "x86_32_internal.h"
#include "../common/wide_mir.h"
#include "../common/narrow_mir.h"
#include "../common/ir_lower.h"
#include "anvil/anvil_analysis.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    anvil_ir_lower_t base;
    const anvil_x86_cc_desc_t *desc;
    const anvil_x86_plat_desc_t *plat;
    /* Bytes RET drops from the stack (callee-cleanup conventions and
     * popped hidden result pointers). */
    int64_t ret_pop;
    /* Stack bytes of the named parameters (va_start). */
    int64_t named_stack_bytes;
    /* SRET result: in registers (RET loads it from sret_object) or written
     * through the hidden pointer, which RET returns in EAX. */
    bool sret_in_regs;
    x86_agg_plan_t sret_plan;
    anvil_mir_vreg_t sret_object;
    anvil_mir_vreg_t sret_pointer;
} x86_mir_lower_t;

static bool x86_type_is_void(anvil_type_t *type)
{
    return !type || type->kind == ANVIL_TYPE_VOID;
}

static bool x86_type_is_signed(anvil_type_t *type)
{
    return type && type->is_signed && !x86_type_is_float(type);
}

/* Vectors (SSE2 CPUs) share the XMM registers with scalar FP values. */
static anvil_mir_reg_class_t x86_reg_class_for_type(anvil_type_t *type)
{
    return x86_type_is_float(type) || (type && type->kind == ANVIL_TYPE_VECTOR) ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR;
}

static uint16_t x86_bits_for_type(anvil_type_t *type)
{
    if (!type)
        return 32;
    if (type->kind == ANVIL_TYPE_PTR)
        return 32;
    if (x86_type_is_float(type)) {
        return type->kind == ANVIL_TYPE_F64 ? 64 : 32;
    }
    if (type->kind == ANVIL_TYPE_VECTOR)
        return (uint16_t)(anvil_type_size(type) * 8);

    size_t size = anvil_type_size(type);
    if (size == 0)
        return 32;
    if (size >= 4)
        return 32;
    return (uint16_t)(size * 8);
}

static anvil_mir_vreg_t x86_add_vreg_for_type(x86_mir_lower_t *lower, anvil_type_t *type)
{
    if (type && type->kind == ANVIL_TYPE_VECTOR && !anvil_vector_operation_cost(lower->base.func->owner_ctx, ANVIL_OP_LOAD, type))
        return ANVIL_MIR_NO_VREG;
    return anvil_mir_add_vreg_typed(lower->base.mir, x86_reg_class_for_type(type), x86_bits_for_type(type), x86_type_is_signed(type));
}

static anvil_mir_vreg_t x86_add_i32_vreg(x86_mir_lower_t *lower, bool is_signed)
{
    return anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 32, is_signed);
}

static bool x86_arg_uses_int_reg(const anvil_x86_cc_desc_t *desc, anvil_type_t *type, size_t int_reg_count)
{
    if (x86_type_is_float(type) || x86_needs_pair(type))
        return false;
    return (int)int_reg_count < desc->num_reg_int_args;
}

static bool lower_params(x86_mir_lower_t *lower)
{
    const anvil_x86_cc_desc_t *desc = lower->desc;
    int64_t stack_offset = 0;
    size_t int_reg_count = 0;
    bool hidden_result_pointer = false;


    for (size_t i = 0; i < lower->base.func->num_params; i++) {
        anvil_value_t *param = lower->base.func->params[i];
        if (!param)
            return false;

        anvil_param_attr_t attr = anvil_type_func_param_attr(lower->base.func->type, i);
        if (attr != ANVIL_PARAM_PLAIN) {
            x86_agg_plan_t plan;
            if (!x86_classify_aggregate(lower->plat, param->type->data.pointee, attr == ANVIL_PARAM_SRET, &plan))
                return false;
            if (plan.placement != X86_AGG_INDIRECT) {
                /* The callee's own copy of a stack argument, or the local
                 * result object RET loads into the result registers. */
                anvil_mir_vreg_t object = anvil_agg_frame_object(lower->base.mir, plan.size, plan.align, x86_agg_target);
                if (object == ANVIL_MIR_NO_VREG || !anvil_lower_vreg_put(&lower->base.values, param, object))
                    return false;
                if (attr == ANVIL_PARAM_SRET) {
                    lower->sret_in_regs = true;
                    lower->sret_plan = plan;
                    lower->sret_object = object;
                    continue;
                }
                for (size_t offset = 0; offset < plan.size; offset += 4) {
                    anvil_agg_part_t chunk = anvil_agg_chunk(plan.size, offset, x86_agg_target);
                    anvil_mir_vreg_t value = x86_add_i32_vreg(lower, false);
                    if (value == ANVIL_MIR_NO_VREG ||
                        !anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, value, stack_offset + (int64_t)offset) ||
                        !anvil_agg_store_part(lower->base.mir, &chunk, value, object, x86_agg_target))
                        return false;
                }
                stack_offset += (int64_t)((plan.size + 3) & ~(size_t)3);
                continue;
            }
            /* The hidden result pointer is an ordinary pointer argument. */
            hidden_result_pointer = true;
        }

        if (x86_needs_pair(param->type)) {
            bool is_unsigned = param->type->kind == ANVIL_TYPE_U64;
            anvil_mir_vreg_t lo = x86_add_i32_vreg(lower, false);
            anvil_mir_vreg_t hi = x86_add_i32_vreg(lower, !is_unsigned);
            if (lo == ANVIL_MIR_NO_VREG || hi == ANVIL_MIR_NO_VREG)
                return false;
            if (!anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, lo, stack_offset)) {
                return false;
            }
            if (!anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, hi, stack_offset + 4)) {
                return false;
            }
            stack_offset += 8;
            if (!anvil_lower_pair_put(&lower->base.pairs, param, hi, lo, is_unsigned))
                return false;
            continue;
        }

        anvil_mir_vreg_t local = x86_add_vreg_for_type(lower, param->type);
        if (local == ANVIL_MIR_NO_VREG)
            return false;

        if (x86_arg_uses_int_reg(desc, param->type, int_reg_count)) {
            int reg = desc->reg_int_args[int_reg_count];
            anvil_mir_vreg_t incoming = x86_add_vreg_for_type(lower, param->type);
            if (incoming == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->base.mir, incoming, reg) || !anvil_mir_set_live_in(lower->base.mir, incoming, true)) {
                return false;
            }
            anvil_mir_vreg_t uses[] = {incoming};
            if (!anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, local, uses, 1)) {
                return false;
            }
            int_reg_count++;
        } else {
            if (!anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, local, stack_offset)) {
                return false;
            }
            stack_offset += x86_stack_arg_slot_size(param->type);
        }

        if (attr == ANVIL_PARAM_SRET)
            lower->sret_pointer = local;
        if (!anvil_lower_vreg_put(&lower->base.values, param, local))
            return false;
    }

    lower->ret_pop = x86_callee_pop_bytes(desc, lower->plat, stack_offset, hidden_result_pointer);
    lower->named_stack_bytes = stack_offset;
    return true;
}

static bool emit_vreg_copy(anvil_mir_func_t *mir, anvil_mir_vreg_t dst, anvil_mir_vreg_t src);

/* Run-time helpers for 64-bit arithmetic, called with cdecl: arguments in
 * the outgoing area (pairs low word first), results in EAX:EDX or ST(0). */
static bool x86_mark_symbol_access(x86_mir_lower_t *lower, const anvil_value_t *source);

static bool x86_wide_call_helper(void *ctx, const char *name, const anvil_wide_pair_t *args, size_t num_args,
                                 anvil_wide_pair_t result)
{
    x86_mir_lower_t *lower = ctx;
    int64_t offset = 0;
    for (size_t i = 0; i < num_args; i++) {
        anvil_mir_vreg_t words[2] = {args[i].lo, args[i].hi};
        size_t count = args[i].hi != ANVIL_MIR_NO_VREG ? 2 : 1;
        for (size_t w = 0; w < count; w++) {
            const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(lower->base.mir, words[w]);
            anvil_mir_vreg_t uses[] = {words[w]};
            if (!info || !anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, uses, 1, offset))
                return false;
            offset += info->reg_class == ANVIL_MIR_REG_FPR && info->size_bits > 32 ? 8 : 4;
        }
    }

    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(lower->base.mir, result.lo);
    if (!info)
        return false;
    if (info->reg_class == ANVIL_MIR_REG_FPR)
        return anvil_mir_add_call(lower->base.mir, ANVIL_MIR_NO_VREG, NULL, 0, name, ANVIL_CC_CDECL, false, 0) && x86_mark_symbol_access(lower, NULL) &&
               anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_CALL_RESULT, result.lo, NULL, 0);
    if (!anvil_mir_add_call(lower->base.mir, result.lo, NULL, 0, name, ANVIL_CC_CDECL, false, 0) || !x86_mark_symbol_access(lower, NULL))
        return false;
    return result.hi == ANVIL_MIR_NO_VREG || anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_CALL_RESULT, result.hi, NULL, 0);
}

/* The last instruction names `source` (NULL: a run-time helper): through
 * the GOT/PLT when the relocation model requires (helpers whenever the code
 * is position-independent). */
/* anvil_ir_mark_symbol, and a run-time helper (no `source`) through the
 * PLT whenever the code is position-independent. */
static bool x86_mark_symbol_access(x86_mir_lower_t *lower, const anvil_value_t *source)
{
    anvil_ctx_t *ctx = lower->base.func->parent ? lower->base.func->parent->ctx : NULL;
    if (source)
        return anvil_ir_mark_symbol(&lower->base, source);
    return anvil_ctx_get_reloc_model(ctx) == ANVIL_RELOC_STATIC || anvil_mir_set_instr_symbol_got(lower->base.mir, anvil_mir_num_instrs(lower->base.mir) - 1, true);
}

/* RET with the bytes this function drops from the stack. */
static bool add_ret(x86_mir_lower_t *lower, anvil_mir_vreg_t value)
{
    anvil_mir_vreg_t uses[] = {value};
    size_t num_uses = value == ANVIL_MIR_NO_VREG ? 0 : 1;
    return lower->ret_pop > 0 ? anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, uses, num_uses, lower->ret_pop)
                              : anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, uses, num_uses);
}

/* A general register result as a copy in the register the convention
 * returns it in (copy hints then compute it there); a floating one leaves
 * through ST(0) as it is. */
static anvil_mir_vreg_t x86_in_return_reg(x86_mir_lower_t *lower, anvil_mir_vreg_t value, int reg)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(lower->base.mir, value);
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR)
        return value;
    anvil_mir_reg_class_t reg_class = info->reg_class;
    uint16_t bits = info->size_bits;
    bool is_signed = info->is_signed;
    anvil_mir_vreg_t fixed = anvil_mir_add_vreg_typed(lower->base.mir, reg_class, bits, is_signed);
    return fixed != ANVIL_MIR_NO_VREG && anvil_mir_set_fixed_reg(lower->base.mir, fixed, reg) && emit_vreg_copy(lower->base.mir, fixed, value) ? fixed
                                                                                                                                  : ANVIL_MIR_NO_VREG;
}

/* RET of `lo` (EAX or ST(0)), after RET_VALUE_PART of `hi` (EDX) when
 * given; both copies come before the pair, which stays adjacent. */
static bool add_ret_values(x86_mir_lower_t *lower, anvil_mir_vreg_t lo, anvil_mir_vreg_t hi)
{
    if (lo == ANVIL_MIR_NO_VREG)
        return false;
    anvil_mir_vreg_t lo_ret = x86_in_return_reg(lower, lo, lower->desc->int_ret_reg);
    anvil_mir_vreg_t hi_ret = hi != ANVIL_MIR_NO_VREG ? x86_in_return_reg(lower, hi, lower->desc->int_ret_hi_reg) : ANVIL_MIR_NO_VREG;
    if (lo_ret == ANVIL_MIR_NO_VREG || (hi != ANVIL_MIR_NO_VREG && hi_ret == ANVIL_MIR_NO_VREG))
        return false;
    if (hi_ret != ANVIL_MIR_NO_VREG && !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_RET_VALUE_PART, ANVIL_MIR_NO_VREG, &hi_ret, 1))
        return false;
    return add_ret(lower, lo_ret);
}

static bool add_return(x86_mir_lower_t *lower, anvil_value_t *value)
{
    if (!value && lower->sret_in_regs) {
        /* EAX[:EDX] or ST(0), loaded from the local result object. */
        const x86_agg_plan_t *plan = &lower->sret_plan;
        anvil_mir_vreg_t parts[2] = {ANVIL_MIR_NO_VREG, ANVIL_MIR_NO_VREG};
        for (unsigned part = 0; part < plan->num_parts; part++) {
            parts[part] = anvil_agg_load_part(lower->base.mir, &plan->parts[part], lower->sret_object, x86_agg_target);
            if (parts[part] == ANVIL_MIR_NO_VREG)
                return false;
        }
        return add_ret_values(lower, parts[0], parts[1]);
    }
    if (!value) {
        /* A hidden result pointer comes back in EAX. */
        return lower->sret_pointer == ANVIL_MIR_NO_VREG ? add_ret(lower, ANVIL_MIR_NO_VREG)
                                                         : add_ret_values(lower, lower->sret_pointer, ANVIL_MIR_NO_VREG);
    }

    if (x86_needs_pair(value->type)) {
        anvil_wide_pair_t pair;
        return anvil_ir_value_pair(&lower->base, value, &pair) && add_ret_values(lower, pair.lo, pair.hi);
    }

    return add_ret_values(lower, anvil_ir_lower_value(&lower->base, value), ANVIL_MIR_NO_VREG);
}

static bool emit_vreg_copy(anvil_mir_func_t *mir, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    anvil_mir_vreg_t uses[] = {src};
    return anvil_mir_add_instr(mir, ANVIL_MIR_OP_COPY, dst, uses, 1);
}

static bool lower_call(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    const anvil_x86_cc_desc_t *desc = anvil_x86_get_cc_desc(instr->call_cc);
    if (!desc)
        return false;
    if (instr->num_operands == 0)
        return false;

    anvil_value_t *callee = instr->operands[0];
    anvil_type_t *fn_type = anvil_ir_call_type(callee);
    if (!fn_type)
        return false;

    bool direct_call = anvil_ir_call_is_direct(callee);
    const char *symbol = direct_call ? anvil_ir_call_symbol(callee) : NULL;
    if (direct_call && !symbol)
        return false;

    size_t num_args = instr->num_operands - 1;
    size_t max_call_uses = num_args + (direct_call ? 0 : 1) + 1;
    bool hidden_result_pointer = false;
    bool sret_in_regs = false;
    x86_agg_plan_t sret_plan;
    anvil_mir_vreg_t sret_object = ANVIL_MIR_NO_VREG;
    const anvil_x86_plat_desc_t *plat = lower->plat;
    anvil_mir_vreg_t *call_uses = NULL;
    if (max_call_uses > 0) {
        call_uses = calloc(max_call_uses, sizeof(*call_uses));
        if (!call_uses)
            return false;
    }

    size_t int_reg_count = 0;
    size_t num_call_uses = 0;
    int64_t stack_offset = 0;
    bool ok = true;
    /* Where each register argument sits among the call's uses. */
    enum { X86_MAX_REG_ARGS = 4 };
    size_t reg_arg_at[X86_MAX_REG_ARGS];
    if (desc->num_reg_int_args > X86_MAX_REG_ARGS) {
        free(call_uses);
        return false;
    }

    if (!direct_call) {
        anvil_mir_vreg_t target_src = anvil_ir_lower_value(&lower->base, callee);
        if (target_src == ANVIL_MIR_NO_VREG) {
            free(call_uses);
            return false;
        }
        call_uses[num_call_uses++] = target_src;
    }

    for (size_t i = 0; i < num_args; i++) {
        anvil_value_t *arg = instr->operands[i + 1];

        if (x86_needs_pair(arg->type)) {
            anvil_wide_pair_t pair;
            if (!anvil_ir_value_pair(&lower->base, arg, &pair)) {
                ok = false;
                break;
            }
            anvil_mir_vreg_t lo_use[] = {pair.lo};
            anvil_mir_vreg_t hi_use[] = {pair.hi};
            if (!anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, lo_use, 1, stack_offset) ||
                !anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, hi_use, 1, stack_offset + 4)) {
                ok = false;
                break;
            }
            stack_offset += 8;
            continue;
        }

        anvil_mir_vreg_t src = anvil_ir_lower_value(&lower->base, arg);
        if (src == ANVIL_MIR_NO_VREG) {
            ok = false;
            break;
        }

        anvil_param_attr_t attr = anvil_call_arg_attr(instr, fn_type, i);
        if (attr != ANVIL_PARAM_PLAIN) {
            x86_agg_plan_t plan;
            if (!x86_classify_aggregate(plat, arg->type->data.pointee, attr == ANVIL_PARAM_SRET, &plan)) {
                ok = false;
                break;
            }
            if (plan.placement == X86_AGG_REGS) {
                /* Stored from the result registers after the call. */
                sret_in_regs = true;
                sret_plan = plan;
                sret_object = src;
                continue;
            }
            if (plan.placement == X86_AGG_STACK) {
                /* The memory image, word by word, in the argument area. */
                for (size_t offset = 0; ok && offset < plan.size; offset += 4) {
                    anvil_agg_part_t chunk = anvil_agg_chunk(plan.size, offset, x86_agg_target);
                    anvil_mir_vreg_t value = anvil_agg_load_part(lower->base.mir, &chunk, src, x86_agg_target);
                    anvil_mir_vreg_t stack_use[] = {value};
                    ok = value != ANVIL_MIR_NO_VREG &&
                         anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, stack_use, 1, stack_offset + (int64_t)offset);
                }
                stack_offset += (int64_t)((plan.size + 3) & ~(size_t)3);
                if (!ok)
                    break;
                continue;
            }
            /* The hidden result pointer is an ordinary pointer argument. */
            hidden_result_pointer = true;
        }

        if (x86_arg_uses_int_reg(desc, arg->type, int_reg_count)) {
            reg_arg_at[int_reg_count] = num_call_uses;
            call_uses[num_call_uses++] = src;
            int_reg_count++;
            continue;
        }

        anvil_mir_vreg_t stack_use[] = {src};
        if (!anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, stack_use, 1, stack_offset)) {
            ok = false;
            break;
        }
        stack_offset += x86_stack_arg_slot_size(arg->type);
    }

    /* Register arguments, last: each into a value in its register, so
     * the allocator places them there (copy hints) and the call's moves
     * have nothing left to do. */
    for (size_t r = 0; ok && r < int_reg_count && r < X86_MAX_REG_ARGS; r++) {
        anvil_mir_vreg_t value = call_uses[reg_arg_at[r]];
        const anvil_mir_vreg_info_t *value_info = anvil_mir_get_vreg_info(lower->base.mir, value);
        anvil_mir_vreg_t fixed = value_info ? anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, value_info->size_bits, value_info->is_signed)
                                            : ANVIL_MIR_NO_VREG;
        anvil_mir_vreg_t uses[] = {value};
        ok = fixed != ANVIL_MIR_NO_VREG && anvil_mir_set_fixed_reg(lower->base.mir, fixed, desc->reg_int_args[r]) &&
             anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, fixed, uses, 1);
        call_uses[reg_arg_at[r]] = fixed;
    }
    if (!ok) {
        free(call_uses);
        return false;
    }

    bool result_is_pair = instr->result && x86_needs_pair(instr->result->type);
    bool result_is_fp = instr->result && x86_type_is_float(instr->result->type);
    bool result_is_int = instr->result && !x86_type_is_void(instr->result->type) && !result_is_pair && !result_is_fp;

    anvil_mir_vreg_t call_def = ANVIL_MIR_NO_VREG;
    if (sret_in_regs && !sret_plan.parts[0].fp) {
        call_def = x86_add_i32_vreg(lower, false);
        if (call_def == ANVIL_MIR_NO_VREG) {
            free(call_uses);
            return false;
        }
    } else if (result_is_int || result_is_pair) {
        /* The result register itself, copied out after the call (copy
         * hints then keep the value there when they can). */
        call_def = result_is_int ? x86_add_vreg_for_type(lower, instr->result->type) : x86_add_i32_vreg(lower, false);
        if (call_def == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->base.mir, call_def, desc->int_ret_reg)) {
            free(call_uses);
            return false;
        }
    }

    /* The immediate records the bytes the callee drops from the stack, so
     * the caller can restore its outgoing argument area. */
    int64_t callee_pop = x86_callee_pop_bytes(desc, plat, stack_offset, hidden_result_pointer);
    char decorated[512];
    if (symbol && x86_decorated_symbol(plat, desc, fn_type, symbol, decorated, sizeof(decorated)))
        symbol = decorated;
    bool emit_ok = anvil_mir_add_call(lower->base.mir, call_def, call_uses, num_call_uses, symbol, instr->call_cc, callee_pop > 0, callee_pop) &&
                   (!symbol || x86_mark_symbol_access(lower, callee));
    free(call_uses);
    if (!emit_ok)
        return false;

    if (sret_in_regs) {
        anvil_mir_vreg_t parts[2] = {call_def, ANVIL_MIR_NO_VREG};
        if (sret_plan.parts[0].fp) {
            /* ST(0) */
            parts[0] = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_FPR, anvil_agg_part_bits(&sret_plan.parts[0], x86_agg_target), false);
            if (parts[0] == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_CALL_RESULT, parts[0], NULL, 0))
                return false;
        } else if (sret_plan.num_parts == 2) {
            /* EDX */
            parts[1] = x86_add_i32_vreg(lower, false);
            if (parts[1] == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_CALL_RESULT, parts[1], NULL, 0))
                return false;
        }
        for (unsigned part = 0; part < sret_plan.num_parts; part++)
            if (!anvil_agg_store_part(lower->base.mir, &sret_plan.parts[part], parts[part], sret_object, x86_agg_target))
                return false;
        return true;
    }

    if (!instr->result || x86_type_is_void(instr->result->type)) {
        return true;
    }

    if (result_is_pair) {
        bool is_unsigned = instr->result->type->kind == ANVIL_TYPE_U64;
        anvil_mir_vreg_t hi = x86_add_i32_vreg(lower, !is_unsigned);
        anvil_mir_vreg_t lo_local = x86_add_i32_vreg(lower, false), hi_local = x86_add_i32_vreg(lower, !is_unsigned);
        if (hi == ANVIL_MIR_NO_VREG || lo_local == ANVIL_MIR_NO_VREG || hi_local == ANVIL_MIR_NO_VREG ||
            !anvil_mir_set_fixed_reg(lower->base.mir, hi, desc->int_ret_hi_reg))
            return false;
        if (!anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_CALL_RESULT, hi, NULL, 0) || !emit_vreg_copy(lower->base.mir, lo_local, call_def) ||
            !emit_vreg_copy(lower->base.mir, hi_local, hi)) {
            return false;
        }
        return anvil_lower_pair_put(&lower->base.pairs, instr->result, hi_local, lo_local, is_unsigned);
    }

    if (result_is_fp) {
        anvil_mir_vreg_t local_result = x86_add_vreg_for_type(lower, instr->result->type);
        if (local_result == ANVIL_MIR_NO_VREG)
            return false;
        if (!anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_CALL_RESULT, local_result, NULL, 0)) {
            return false;
        }
        return anvil_lower_vreg_put(&lower->base.values, instr->result, local_result);
    }

    anvil_mir_vreg_t local = x86_add_vreg_for_type(lower, instr->result->type);
    return local != ANVIL_MIR_NO_VREG && emit_vreg_copy(lower->base.mir, local, call_def) && anvil_lower_vreg_put(&lower->base.values, instr->result, local);
}

static bool target_instr(anvil_ir_lower_t *base, anvil_instr_t *instr)
{
    x86_mir_lower_t *lower = (x86_mir_lower_t *)base;
    switch (instr->op) {
    case ANVIL_OP_ATOMIC_LOAD:
    case ANVIL_OP_ATOMIC_STORE:
    case ANVIL_OP_ATOMIC_RMW:
    case ANVIL_OP_ATOMIC_CMPXCHG:
    case ANVIL_OP_ATOMIC_FENCE: {
        anvil_ctx_t *ctx = lower->base.func->parent ? lower->base.func->parent->ctx : NULL;
        anvil_cpu_features_t features = ctx ? ctx->cpu_features : anvil_cpu_model_features(anvil_arch_default_cpu(ANVIL_ARCH_X86));
        bool needs_i486 = instr->op == ANVIL_OP_ATOMIC_CMPXCHG ||
                          (instr->op == ANVIL_OP_ATOMIC_RMW && instr->atomic.rmw != ANVIL_ATOMIC_EXCHANGE);
        if (needs_i486 && !(features & ANVIL_FEATURE_X86_I486)) {
            if (ctx)
                anvil_set_error(ctx, ANVIL_ERR_CODEGEN, "x86 atomic read-modify-write needs CMPXCHG/XADD (i486 or later CPU model)");
            return false;
        }

        anvil_mir_vreg_t uses[3];
        for (size_t operand = 0; operand < instr->num_operands && operand < 3; operand++) {
            if (x86_needs_pair(instr->operands[operand]->type)) {
                if (ctx)
                    anvil_set_error(ctx, ANVIL_ERR_CODEGEN, "x86 atomics on 64-bit values are not supported");
                return false;
            }
            uses[operand] = anvil_ir_lower_value(&lower->base, instr->operands[operand]);
            if (uses[operand] == ANVIL_MIR_NO_VREG)
                return false;
        }

        anvil_mir_vreg_t def = instr->result ? x86_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
        if ((instr->result && def == ANVIL_MIR_NO_VREG) || instr->num_operands > 3 ||
            !anvil_mir_add_atomic(lower->base.mir, instr->op, def, uses, instr->num_operands, &instr->atomic))
            return false;
        return !instr->result || anvil_lower_vreg_put(&lower->base.values, instr->result, def);
    }

    case ANVIL_OP_VA_START: {
        /* The first unnamed argument follows the named stack arguments. */
        anvil_mir_vreg_t def = instr->result ? x86_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
        return def != ANVIL_MIR_NO_VREG && lower->base.func->type->data.func.variadic &&
               anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_VA_START, def, lower->named_stack_bytes) &&
               anvil_lower_vreg_put(&lower->base.values, instr->result, def);
    }
    case ANVIL_OP_CALL:
        return lower_call(lower, instr);
    case ANVIL_OP_RET:
        if (instr->num_operands == 0)
            return add_return(lower, NULL);
        if (instr->num_operands == 1)
            return add_return(lower, instr->operands[0]);
        return false;
    default:
        return false;
    }
}

/* anvil_ir_lower_t hooks: a 64-bit integer in a word pair. */
static anvil_mir_vreg_t ir_vreg_for_type(anvil_ir_lower_t *base, anvil_type_t *type)
{
    return x86_add_vreg_for_type((x86_mir_lower_t *)base, type);
}

static bool ir_lower_params(anvil_ir_lower_t *base)
{
    return lower_params((x86_mir_lower_t *)base);
}

static bool ir_pair_type(const anvil_ir_lower_t *base, anvil_type_t *type)
{
    (void)base;
    return x86_needs_pair(type);
}

anvil_mir_func_t *anvil_x86_lower_func_to_mir(anvil_func_t *func)
{
    if (!func || func->is_declaration || !func->type || func->type->kind != ANVIL_TYPE_FUNC)
        return NULL;

    const anvil_x86_cc_desc_t *desc = anvil_x86_get_cc_desc(func->type->data.func.cc);
    if (!desc)
        return NULL;

    x86_mir_lower_t lower;
    memset(&lower, 0, sizeof(lower));
    lower.desc = desc;
    lower.plat = anvil_x86_get_plat_desc(func->parent && func->parent->ctx ? func->parent->ctx->abi : ANVIL_ABI_DEFAULT);
    lower.base.func = func;
    lower.base.vreg_for_type = ir_vreg_for_type;
    lower.base.lower_params = ir_lower_params;
    lower.base.target_instr = target_instr;
    lower.base.pointer_bits = 32;
    lower.base.stack_alignment = (unsigned)lower.plat->stack_alignment;
    lower.base.pair_type = ir_pair_type;
    lower.base.wide = (anvil_wide_target_t){x86_wide_call_helper, &lower};
    lower.sret_pointer = ANVIL_MIR_NO_VREG;
    /* CMOVcc selects general registers; without it, and for FP values, a select branches. */
    if (anvil_ctx_get_cpu_features(func->owner_ctx) & ANVIL_FEATURE_X86_CMOV)
        lower.base.select_classes = 1u << ANVIL_MIR_REG_GPR;
    return anvil_ir_lower_function(&lower.base);
}
