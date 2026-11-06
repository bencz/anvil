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
    anvil_lower_map_t pairs;
    anvil_lower_map_t wide_consts;
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

static size_t x86_slot_bytes_for_type(anvil_type_t *type)
{
    size_t size = type ? x86_type_size(type) : 4;
    return size ? size : 4;
}

static uint16_t x86_align_for_type(anvil_type_t *type)
{
    int align = type ? x86_type_align(type) : 4;
    if (align <= 0)
        align = 4;
    if (align > UINT16_MAX)
        align = UINT16_MAX;
    return (uint16_t)align;
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

static anvil_mir_vreg_t x86_add_bool_vreg(x86_mir_lower_t *lower)
{
    return anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 8, false);
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
            if (!anvil_lower_pair_put(&lower->pairs, param, hi, lo, is_unsigned))
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

static bool prepare_phi_results(x86_mir_lower_t *lower)
{
    for (anvil_block_t *block = lower->base.func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_PHI)
                break;
            if (!instr->result)
                return false;
            if (x86_needs_pair(instr->result->type)) {
                anvil_wide_pair_t pair;
                if (!anvil_wide_new(lower->base.mir, anvil_wide_value_unsigned(instr->result), &pair) || !anvil_lower_wide_put(&lower->pairs, instr->result, pair))
                    return false;
                continue;
            }

            anvil_mir_vreg_t vreg = x86_add_vreg_for_type(lower, instr->result->type);
            if (vreg == ANVIL_MIR_NO_VREG)
                return false;
            if (!anvil_lower_vreg_put(&lower->base.values, instr->result, vreg))
                return false;
        }
    }

    return true;
}

static int64_t float_bits_as_i64(double value, uint16_t bits)
{
    if (bits == 32) {
        float f = (float)value;
        uint32_t raw = 0;
        memcpy(&raw, &f, sizeof(raw));
        return (int64_t)raw;
    }

    uint64_t raw = 0;
    memcpy(&raw, &value, sizeof(raw));
    return (int64_t)raw;
}

static anvil_mir_vreg_t lower_value(x86_mir_lower_t *lower, anvil_value_t *value);
static bool lower_add_const_offset(x86_mir_lower_t *lower, anvil_mir_vreg_t base, int64_t offset, anvil_mir_vreg_t *out_ptr);
static bool emit_vreg_copy(anvil_mir_func_t *mir, anvil_mir_vreg_t dst, anvil_mir_vreg_t src);

static anvil_mir_vreg_t lower_const_value(x86_mir_lower_t *lower, anvil_value_t *value)
{
    anvil_mir_vreg_t vreg = x86_add_vreg_for_type(lower, value->type);
    if (vreg == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;

    int64_t imm = 0;
    switch (value->kind) {
    case ANVIL_VAL_CONST_INT:
        imm = value->data.i;
        break;
    case ANVIL_VAL_CONST_NULL:
        imm = 0;
        break;
    case ANVIL_VAL_CONST_FLOAT:
        imm = float_bits_as_i64(value->data.f, x86_bits_for_type(value->type));
        break;
    default:
        return ANVIL_MIR_NO_VREG;
    }

    if (!anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_MOV, vreg, imm)) {
        return ANVIL_MIR_NO_VREG;
    }
    return vreg;
}

static bool ensure_i64_pair(x86_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *hi, anvil_mir_vreg_t *lo, bool *is_unsigned)
{
    if (value && value->kind == ANVIL_VAL_CONST_INT) {
        /* Materialized at each use: a constant may be used in blocks the
         * first use does not dominate. */
        bool value_unsigned = value->type && value->type->kind == ANVIL_TYPE_U64;
        anvil_wide_pair_t pair;
        if (!anvil_wide_constant(lower->base.mir, value->data.i, value_unsigned, &pair))
            return false;
        *hi = pair.hi;
        *lo = pair.lo;
        if (is_unsigned)
            *is_unsigned = value_unsigned;
        return true;
    }
    return anvil_lower_pair_get(&lower->pairs, value, hi, lo, is_unsigned);
}

static bool ensure_wide(x86_mir_lower_t *lower, anvil_value_t *value, anvil_wide_pair_t *pair)
{
    return ensure_i64_pair(lower, value, &pair->hi, &pair->lo, NULL);
}

static bool lower_wide_binary(x86_mir_lower_t *lower, anvil_instr_t *instr, anvil_mir_opcode_t mir_op);
static anvil_mir_vreg_t lower_widen_gpr_to_32(x86_mir_lower_t *lower, anvil_mir_vreg_t src, bool sign_extend);

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

static anvil_wide_target_t x86_wide_target(x86_mir_lower_t *lower)
{
    return (anvil_wide_target_t){x86_wide_call_helper, lower};
}

/* The last instruction names `source` (NULL: a run-time helper): through
 * the GOT/PLT when the relocation model requires (helpers whenever the code
 * is position-independent). */
static bool x86_mark_symbol_access(x86_mir_lower_t *lower, const anvil_value_t *source)
{
    anvil_ctx_t *ctx = lower->base.func->parent ? lower->base.func->parent->ctx : NULL;
    bool got = source ? anvil_symbol_uses_got(ctx, source) : anvil_ctx_get_reloc_model(ctx) != ANVIL_RELOC_STATIC;
    return !got || anvil_mir_set_instr_symbol_got(lower->base.mir, anvil_mir_num_instrs(lower->base.mir) - 1, true);
}

static anvil_mir_vreg_t lower_symbol_address(x86_mir_lower_t *lower, const char *symbol, const anvil_value_t *source)
{
    if (!symbol || !symbol[0])
        return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t vreg = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 32, false);
    if (vreg == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;

    if (!anvil_mir_add_instr_symbol(lower->base.mir, ANVIL_MIR_OP_SYMBOL_ADDR, vreg, NULL, 0, symbol) ||
        (source && !x86_mark_symbol_access(lower, source))) {
        return ANVIL_MIR_NO_VREG;
    }
    return vreg;
}

static anvil_mir_vreg_t lower_reloc_address(x86_mir_lower_t *lower, anvil_value_t *value)
{
    const char *symbol = value && value->data.reloc.symbol ? value->data.reloc.symbol->name : NULL;
    if (!symbol || !symbol[0])
        return ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t vreg = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 32, false);
    if (vreg == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_symbol_imm(lower->base.mir, ANVIL_MIR_OP_SYMBOL_ADDR, vreg, NULL, 0, symbol, value->data.reloc.addend) ||
        !x86_mark_symbol_access(lower, value->data.reloc.symbol))
        return ANVIL_MIR_NO_VREG;
    return vreg;
}

static anvil_mir_vreg_t lower_string_address(x86_mir_lower_t *lower, anvil_value_t *value)
{
    const char *label = NULL;
    if (anvil_mir_add_string_literal(lower->base.mir, value->data.str, &label) < 0 || !label) {
        return ANVIL_MIR_NO_VREG;
    }
    return lower_symbol_address(lower, label, NULL);
}

static anvil_mir_vreg_t lower_value(x86_mir_lower_t *lower, anvil_value_t *value)
{
    if (!value)
        return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t existing = anvil_lower_vreg_get(&lower->base.values, value);
    if (existing != ANVIL_MIR_NO_VREG)
        return existing;

    anvil_mir_vreg_t base = ANVIL_MIR_NO_VREG;
    int64_t offset = 0;
    if (anvil_lower_address_get(&lower->base.addresses, value, &base, &offset)) {
        anvil_mir_vreg_t materialized = ANVIL_MIR_NO_VREG;
        if (!lower_add_const_offset(lower, base, offset, &materialized)) {
            return ANVIL_MIR_NO_VREG;
        }
        /* Materialized where this use is, every time: a vreg from the
         * first use need not dominate the others (the base does). */
        return materialized;
    }

    switch (value->kind) {
    case ANVIL_VAL_CONST_INT:
    case ANVIL_VAL_CONST_FLOAT:
    case ANVIL_VAL_CONST_NULL:
        return lower_const_value(lower, value);
    case ANVIL_VAL_CONST_STRING:
        return lower_string_address(lower, value);
    case ANVIL_VAL_CONST_SYMBOL_ADDR:
    case ANVIL_VAL_CONST_GEP:
        return lower_reloc_address(lower, value);
    case ANVIL_VAL_FUNC:
        if (value->data.func && value->data.func->name) {
            return lower_symbol_address(lower, value->data.func->name, value);
        }
        return lower_symbol_address(lower, value->name, value);
    case ANVIL_VAL_GLOBAL:
        return lower_symbol_address(lower, value->name, value);
    default:
        return ANVIL_MIR_NO_VREG;
    }
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
        anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
        anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
        return ensure_i64_pair(lower, value, &hi, &lo, NULL) && add_ret_values(lower, lo, hi);
    }

    return add_ret_values(lower, lower_value(lower, value), ANVIL_MIR_NO_VREG);
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
        anvil_mir_vreg_t target_src = lower_value(lower, callee);
        if (target_src == ANVIL_MIR_NO_VREG) {
            free(call_uses);
            return false;
        }
        call_uses[num_call_uses++] = target_src;
    }

    for (size_t i = 0; i < num_args; i++) {
        anvil_value_t *arg = instr->operands[i + 1];

        if (x86_needs_pair(arg->type)) {
            anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
            anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
            if (!ensure_i64_pair(lower, arg, &hi, &lo, NULL)) {
                ok = false;
                break;
            }
            anvil_mir_vreg_t lo_use[] = {lo};
            anvil_mir_vreg_t hi_use[] = {hi};
            if (!anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, lo_use, 1, stack_offset) ||
                !anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, hi_use, 1, stack_offset + 4)) {
                ok = false;
                break;
            }
            stack_offset += 8;
            continue;
        }

        anvil_mir_vreg_t src = lower_value(lower, arg);
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
        return anvil_lower_pair_put(&lower->pairs, instr->result, hi_local, lo_local, is_unsigned);
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

static anvil_mir_vreg_t lower_widen_gpr_to_32(x86_mir_lower_t *lower, anvil_mir_vreg_t src, bool sign_extend)
{
    const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->base.mir, src);
    if (!src_info || src_info->reg_class != ANVIL_MIR_REG_GPR) {
        return ANVIL_MIR_NO_VREG;
    }
    if (src_info->size_bits == 32)
        return src;

    if (src_info->size_bits > 32) {
        anvil_mir_vreg_t narrow = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 32, sign_extend);
        if (narrow == ANVIL_MIR_NO_VREG)
            return ANVIL_MIR_NO_VREG;
        anvil_mir_vreg_t uses[] = {src};
        if (!anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_TRUNC, narrow, uses, 1)) {
            return ANVIL_MIR_NO_VREG;
        }
        return narrow;
    }

    anvil_mir_vreg_t wide = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, 32, sign_extend);
    if (wide == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t uses[] = {src};
    anvil_mir_opcode_t op = sign_extend ? ANVIL_MIR_OP_SEXT : ANVIL_MIR_OP_ZEXT;
    if (!anvil_mir_add_instr(lower->base.mir, op, wide, uses, 1)) {
        return ANVIL_MIR_NO_VREG;
    }
    return wide;
}

static anvil_mir_vreg_t lower_resize_gpr(x86_mir_lower_t *lower, anvil_mir_vreg_t src, uint16_t target_bits)
{
    const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->base.mir, src);
    if (!src_info || src_info->reg_class != ANVIL_MIR_REG_GPR || src_info->size_bits == 0 || target_bits == 0) {
        return ANVIL_MIR_NO_VREG;
    }
    if (src_info->size_bits == target_bits)
        return src;

    /* Capture src fields before anvil_mir_add_vreg_typed: it may realloc the
     * vreg array, invalidating src_info. */
    uint16_t src_bits = src_info->size_bits;
    bool src_signed = src_info->is_signed;

    anvil_mir_vreg_t resized = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, target_bits, src_signed);
    if (resized == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t uses[] = {src};
    anvil_mir_opcode_t op = ANVIL_MIR_OP_TRUNC;
    if (src_bits < target_bits) {
        op = src_signed ? ANVIL_MIR_OP_SEXT : ANVIL_MIR_OP_ZEXT;
    }
    if (!anvil_mir_add_instr(lower->base.mir, op, resized, uses, 1)) {
        return ANVIL_MIR_NO_VREG;
    }
    return resized;
}

static bool lower_match_binary_operand_sizes(x86_mir_lower_t *lower, anvil_mir_vreg_t *lhs, anvil_mir_vreg_t *rhs, anvil_mir_vreg_t def)
{
    const anvil_mir_vreg_info_t *def_info = anvil_mir_get_vreg_info(lower->base.mir, def);
    const anvil_mir_vreg_info_t *lhs_info = anvil_mir_get_vreg_info(lower->base.mir, *lhs);
    const anvil_mir_vreg_info_t *rhs_info = anvil_mir_get_vreg_info(lower->base.mir, *rhs);
    if (!def_info || !lhs_info || !rhs_info)
        return false;

    if (def_info->reg_class != ANVIL_MIR_REG_GPR || lhs_info->reg_class != ANVIL_MIR_REG_GPR || rhs_info->reg_class != ANVIL_MIR_REG_GPR) {
        return true;
    }

    /* Capture def size before resizing: lower_resize_gpr may realloc the vreg
     * array, invalidating def_info. */
    uint16_t def_bits = def_info->size_bits;
    *lhs = lower_resize_gpr(lower, *lhs, def_bits);
    *rhs = lower_resize_gpr(lower, *rhs, def_bits);
    return *lhs != ANVIL_MIR_NO_VREG && *rhs != ANVIL_MIR_NO_VREG;
}

static bool lower_alloca(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || !instr->result->type || instr->result->type->kind != ANVIL_TYPE_PTR) {
        return false;
    }

    anvil_type_t *element_type = instr->aux_type;
    if (!element_type) {
        element_type = instr->result->type->data.pointee;
    }
    if (!element_type)
        return false;

    anvil_mir_vreg_t ptr = x86_add_vreg_for_type(lower, instr->result->type);
    if (ptr == ANVIL_MIR_NO_VREG)
        return false;

    if (instr->num_operands == 0) {
        int slot = anvil_mir_add_frame_slot(lower->base.mir, x86_slot_bytes_for_type(element_type), x86_align_for_type(element_type));
        if (slot < 0)
            return false;
        if (!anvil_mir_add_frame_addr(lower->base.mir, ptr, slot))
            return false;
        return anvil_lower_vreg_put(&lower->base.values, instr->result, ptr);
    }

    if (instr->num_operands != 1)
        return false;
    anvil_mir_vreg_t count = lower_value(lower, instr->operands[0]);
    if (count == ANVIL_MIR_NO_VREG)
        return false;
    count = lower_widen_gpr_to_32(lower, count, false);
    if (count == ANVIL_MIR_NO_VREG)
        return false;

    int64_t elem_size = x86_type_size(element_type);
    if (elem_size <= 0)
        elem_size = 1;
    anvil_mir_vreg_t uses[] = {count};
    if (!anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_DYN_ALLOCA, ptr, uses, 1, elem_size)) {
        return false;
    }
    return anvil_lower_vreg_put(&lower->base.values, instr->result, ptr);
}

static bool lower_add_const_offset(x86_mir_lower_t *lower, anvil_mir_vreg_t base, int64_t offset, anvil_mir_vreg_t *out_ptr)
{
    if (offset == 0) {
        *out_ptr = base;
        return true;
    }

    anvil_mir_vreg_t off = anvil_mir_add_vreg_ex(lower->base.mir, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t dst = anvil_mir_add_vreg_ex(lower->base.mir, ANVIL_MIR_REG_GPR, 32);
    if (off == ANVIL_MIR_NO_VREG || dst == ANVIL_MIR_NO_VREG)
        return false;
    if (!anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_MOV, off, offset)) {
        return false;
    }
    anvil_mir_vreg_t uses[] = {base, off};
    if (!anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_ADD, dst, uses, 2)) {
        return false;
    }

    *out_ptr = dst;
    return true;
}

static bool lower_gep(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands < 2 || !instr->result || !instr->aux_type)
        return false;

    anvil_mir_vreg_t current = lower_value(lower, instr->operands[0]);
    if (current == ANVIL_MIR_NO_VREG)
        return false;
    current = lower_widen_gpr_to_32(lower, current, false);
    if (current == ANVIL_MIR_NO_VREG)
        return false;

    anvil_type_t *walk_type = instr->aux_type;
    bool all_constant = true;
    int64_t constant_offset = 0;
    for (size_t i = 1; i < instr->num_operands; i++) {
        anvil_value_t *index_value = instr->operands[i];
        anvil_gep_step_t step;
        if (!anvil_gep_analyze_step(&walk_type, index_value, i - 1, &step) || step.amount > (size_t)INT64_MAX)
            return false;
        if (index_value->kind == ANVIL_VAL_CONST_INT) {
            int64_t offset;
            if (!anvil_gep_const_step_offset(&step, index_value, &offset) || !anvil_gep_accumulate_offset(&constant_offset, offset)) {
                return false;
            }
            continue;
        }
        all_constant = false;
        if (step.kind != ANVIL_GEP_STEP_SCALE)
            return false;
        int64_t elem_size = (int64_t)step.amount;
        anvil_mir_vreg_t index = ANVIL_MIR_NO_VREG;
        if (x86_needs_pair(index_value->type)) {
            anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
            if (!ensure_i64_pair(lower, index_value, &hi, &index, NULL))
                return false;
            /* The low half is exactly the target's modulo-2^32 pointer
             * index. Signedness only affects widening narrower inputs. */
        } else {
            index = lower_value(lower, index_value);
            if (index == ANVIL_MIR_NO_VREG)
                return false;
            index = lower_widen_gpr_to_32(lower, index, index_value->type->is_signed);
        }
        if (index == ANVIL_MIR_NO_VREG)
            return false;

        anvil_mir_vreg_t scaled = index;
        if (elem_size != 1) {
            anvil_mir_vreg_t scale = anvil_mir_add_vreg_ex(lower->base.mir, ANVIL_MIR_REG_GPR, 32);
            scaled = anvil_mir_add_vreg_ex(lower->base.mir, ANVIL_MIR_REG_GPR, 32);
            if (scale == ANVIL_MIR_NO_VREG || scaled == ANVIL_MIR_NO_VREG) {
                return false;
            }
            if (!anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_MOV, scale, elem_size)) {
                return false;
            }
            anvil_mir_vreg_t mul_uses[] = {index, scale};
            if (!anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_MUL, scaled, mul_uses, 2)) {
                return false;
            }
        }

        anvil_mir_vreg_t next = anvil_mir_add_vreg_ex(lower->base.mir, ANVIL_MIR_REG_GPR, 32);
        if (next == ANVIL_MIR_NO_VREG)
            return false;
        anvil_mir_vreg_t add_uses[] = {current, scaled};
        if (!anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_ADD, next, add_uses, 2)) {
            return false;
        }
        current = next;
    }
    if (all_constant)
        return anvil_lower_address_put(&lower->base.addresses, instr->result, current, constant_offset);
    if (!lower_add_const_offset(lower, current, constant_offset, &current))
        return false;
    return anvil_lower_vreg_put(&lower->base.values, instr->result, current);
}

static bool lower_struct_gep(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands < 1 || !instr->result)
        return false;

    anvil_mir_vreg_t base = lower_value(lower, instr->operands[0]);
    if (base == ANVIL_MIR_NO_VREG)
        return false;
    base = lower_widen_gpr_to_32(lower, base, false);
    if (base == ANVIL_MIR_NO_VREG)
        return false;

    int64_t offset = 0;
    if (instr->aux_type && instr->aux_type->kind == ANVIL_TYPE_STRUCT && instr->num_operands > 1 && instr->operands[1] && instr->operands[1]->kind == ANVIL_VAL_CONST_INT) {
        unsigned idx = (unsigned)instr->operands[1]->data.i;
        if (idx >= instr->aux_type->data.struc.num_fields)
            return false;
        offset = (int64_t)instr->aux_type->data.struc.offsets[idx];
    }

    return anvil_lower_address_put(&lower->base.addresses, instr->result, base, offset);
}

static bool lower_cast(x86_mir_lower_t *lower, anvil_instr_t *instr, anvil_mir_opcode_t mir_op)
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
        anvil_mir_vreg_t same = lower_value(lower, instr->operands[0]);
        return same != ANVIL_MIR_NO_VREG && anvil_lower_vreg_put(&lower->base.values, instr->result, same);
    }

    bool dst_pair = x86_needs_pair(instr->result->type);
    bool src_pair = x86_needs_pair(instr->operands[0]->type);
    anvil_wide_target_t target = x86_wide_target(lower);

    if (dst_pair && !src_pair) {
        /* To 64 bits: extension (pointers zero-extend) or FP conversion. */
        anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
        anvil_wide_pair_t out;
        bool is_unsigned = anvil_wide_value_unsigned(instr->result);
        bool ok = src != ANVIL_MIR_NO_VREG &&
                  (mir_op == ANVIL_MIR_OP_FPTOSI || mir_op == ANVIL_MIR_OP_FPTOUI
                       ? anvil_wide_from_fp(lower->base.mir, &target, src, mir_op == ANVIL_MIR_OP_FPTOUI, &out)
                       : anvil_wide_extend(lower->base.mir, src, mir_op == ANVIL_MIR_OP_SEXT, is_unsigned, &out));
        return ok && anvil_lower_wide_put(&lower->pairs, instr->result, out);
    }
    if (src_pair && !dst_pair) {
        /* From 64 bits: the low word (truncation, pointers) or FP. */
        anvil_wide_pair_t a;
        if (!ensure_wide(lower, instr->operands[0], &a))
            return false;
        anvil_mir_vreg_t result;
        if (mir_op == ANVIL_MIR_OP_SITOFP || mir_op == ANVIL_MIR_OP_UITOFP) {
            if (!anvil_wide_to_fp(lower->base.mir, &target, a, mir_op == ANVIL_MIR_OP_UITOFP, x86_bits_for_type(instr->result->type), &result))
                return false;
        } else {
            result = anvil_wide_truncate(lower->base.mir, a, x86_bits_for_type(instr->result->type), x86_type_is_signed(instr->result->type));
        }
        return result != ANVIL_MIR_NO_VREG && anvil_lower_vreg_put(&lower->base.values, instr->result, result);
    }

    if (dst_pair) {
        if (mir_op != ANVIL_MIR_OP_BITCAST)
            return false;
        anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
        anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
        bool is_unsigned = instr->result->type->kind == ANVIL_TYPE_U64;
        if (!ensure_i64_pair(lower, instr->operands[0], &hi, &lo, NULL)) {
            return false;
        }
        int64_t imm = 0;
        if (anvil_lower_constant_get(&lower->wide_consts, instr->operands[0], &imm) && !anvil_lower_constant_put(&lower->wide_consts, instr->result, imm)) {
            return false;
        }
        return anvil_lower_pair_put(&lower->pairs, instr->result, hi, lo, is_unsigned);
    }

    anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t def = x86_add_vreg_for_type(lower, instr->result->type);
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

    anvil_mir_vreg_t uses[] = {src};
    if (!anvil_mir_add_instr(lower->base.mir, mir_op, def, uses, 1))
        return false;
    return anvil_lower_vreg_put(&lower->base.values, instr->result, def);
}

static bool lower_select(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands != 3 || !instr->result)
        return false;

    /* A 64-bit select moves both words on each path. */
    bool wide = x86_needs_pair(instr->result->type);
    anvil_wide_pair_t then_pair = {ANVIL_MIR_NO_VREG, ANVIL_MIR_NO_VREG}, else_pair = then_pair, def_pair = then_pair;
    anvil_mir_vreg_t cond = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t then_v, else_v, def;
    if (wide) {
        if (!ensure_wide(lower, instr->operands[1], &then_pair) || !ensure_wide(lower, instr->operands[2], &else_pair) ||
            !anvil_wide_new(lower->base.mir, anvil_wide_value_unsigned(instr->result), &def_pair))
            return false;
        then_v = then_pair.lo;
        else_v = else_pair.lo;
        def = def_pair.lo;
    } else {
        then_v = lower_value(lower, instr->operands[1]);
        else_v = lower_value(lower, instr->operands[2]);
        def = x86_add_vreg_for_type(lower, instr->result->type);
    }
    if (cond == ANVIL_MIR_NO_VREG || then_v == ANVIL_MIR_NO_VREG || else_v == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) {
        return false;
    }

    /* General registers on a CPU with CMOVcc: a conditional move per word
     * (both values exist already). */
    const anvil_mir_vreg_info_t *def_info = anvil_mir_get_vreg_info(lower->base.mir, def);
    if (def_info && def_info->reg_class == ANVIL_MIR_REG_GPR && (anvil_mir_func_cpu_features(lower->base.mir) & ANVIL_FEATURE_X86_CMOV)) {
        anvil_mir_vreg_t uses[] = {cond, then_v, else_v};
        anvil_mir_vreg_t hi_uses[] = {cond, then_pair.hi, else_pair.hi};
        if (!anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_SELECT, def, uses, 3) ||
            (wide && !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_SELECT, def_pair.hi, hi_uses, 3)))
            return false;
        return wide ? anvil_lower_wide_put(&lower->pairs, instr->result, def_pair) : anvil_lower_vreg_put(&lower->base.values, instr->result, def);
    }

    anvil_mir_block_t source = anvil_mir_current_block(lower->base.mir);
    anvil_mir_block_t then_block = anvil_ir_new_block(&lower->base, instr->parent, "select");
    anvil_mir_block_t else_block = anvil_ir_new_block(&lower->base, instr->parent, "select");
    anvil_mir_block_t join_block = anvil_ir_new_block(&lower->base, instr->parent, "select");
    if (then_block == ANVIL_MIR_NO_BLOCK || else_block == ANVIL_MIR_NO_BLOCK || join_block == ANVIL_MIR_NO_BLOCK) {
        return false;
    }

    if (!anvil_mir_set_current_block(lower->base.mir, source) || !anvil_mir_add_cond_branch(lower->base.mir, cond, then_block, else_block)) {
        return false;
    }

    if (!anvil_mir_set_current_block(lower->base.mir, then_block) || !emit_vreg_copy(lower->base.mir, def, then_v) ||
        (wide && !emit_vreg_copy(lower->base.mir, def_pair.hi, then_pair.hi)) || !anvil_mir_add_branch(lower->base.mir, join_block)) {
        return false;
    }

    if (!anvil_mir_set_current_block(lower->base.mir, else_block) || !emit_vreg_copy(lower->base.mir, def, else_v) ||
        (wide && !emit_vreg_copy(lower->base.mir, def_pair.hi, else_pair.hi)) || !anvil_mir_add_branch(lower->base.mir, join_block)) {
        return false;
    }

    if (!anvil_mir_set_current_block(lower->base.mir, join_block))
        return false;
    return wide ? anvil_lower_wide_put(&lower->pairs, instr->result, def_pair) : anvil_lower_vreg_put(&lower->base.values, instr->result, def);
}

static bool lower_memory_address(x86_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *out_base, int64_t *out_offset)
{
    anvil_mir_vreg_t base = ANVIL_MIR_NO_VREG;
    int64_t offset = 0;
    if (anvil_lower_address_get(&lower->base.addresses, value, &base, &offset)) {
        base = lower_widen_gpr_to_32(lower, base, false);
        if (base == ANVIL_MIR_NO_VREG)
            return false;
        *out_base = base;
        *out_offset = offset;
        return true;
    }

    base = lower_value(lower, value);
    if (base == ANVIL_MIR_NO_VREG)
        return false;
    base = lower_widen_gpr_to_32(lower, base, false);
    if (base == ANVIL_MIR_NO_VREG)
        return false;

    *out_base = base;
    *out_offset = 0;
    return true;
}

static bool lower_load_i64_pair(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || instr->num_operands != 1)
        return false;

    anvil_mir_vreg_t base = ANVIL_MIR_NO_VREG;
    int64_t offset = 0;
    if (!lower_memory_address(lower, instr->operands[0], &base, &offset)) {
        return false;
    }
    if (offset > INT64_MAX - 4)
        return false;

    bool is_unsigned = instr->result->type && instr->result->type->kind == ANVIL_TYPE_U64;
    anvil_mir_vreg_t lo = x86_add_i32_vreg(lower, false);
    anvil_mir_vreg_t hi = x86_add_i32_vreg(lower, !is_unsigned);
    if (lo == ANVIL_MIR_NO_VREG || hi == ANVIL_MIR_NO_VREG)
        return false;

    anvil_mir_vreg_t lo_uses[] = {base};
    bool ok = offset == 0 ? anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_LOAD, lo, lo_uses, 1) : anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_LOAD, lo, lo_uses, 1, offset);
    if (!ok)
        return false;

    anvil_mir_vreg_t hi_uses[] = {base};
    if (!anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_LOAD, hi, hi_uses, 1, offset + 4)) {
        return false;
    }

    return anvil_lower_pair_put(&lower->pairs, instr->result, hi, lo, is_unsigned);
}

static bool lower_store_i64_pair(x86_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t base, int64_t offset)
{
    if (offset > INT64_MAX - 4)
        return false;

    anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
    if (!ensure_i64_pair(lower, value, &hi, &lo, NULL))
        return false;

    anvil_mir_vreg_t lo_uses[] = {lo, base};
    bool ok = offset == 0 ? anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, lo_uses, 2)
                          : anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, lo_uses, 2, offset);
    if (!ok)
        return false;

    anvil_mir_vreg_t hi_uses[] = {hi, base};
    return anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, hi_uses, 2, offset + 4);
}

/* 64-bit binary operation through the shared wide-integer legalizer. */
static bool lower_wide_binary(x86_mir_lower_t *lower, anvil_instr_t *instr, anvil_mir_opcode_t mir_op)
{
    if (!instr->result)
        return false;
    anvil_wide_pair_t a, b, out;
    bool shift = instr->op == ANVIL_OP_SHL || instr->op == ANVIL_OP_SHR || instr->op == ANVIL_OP_SAR;
    if (!ensure_wide(lower, instr->operands[0], &a))
        return false;
    if (shift && !x86_needs_pair(instr->operands[1]->type)) {
        /* A shift count may be narrower than the shifted value. */
        b.hi = ANVIL_MIR_NO_VREG;
        b.lo = lower_widen_gpr_to_32(lower, lower_value(lower, instr->operands[1]), false);
        if (b.lo == ANVIL_MIR_NO_VREG)
            return false;
    } else if (!ensure_wide(lower, instr->operands[1], &b)) {
        return false;
    }
    if (anvil_ir_compare_op(mir_op)) {
        anvil_mir_vreg_t result = x86_add_bool_vreg(lower);
        return result != ANVIL_MIR_NO_VREG && anvil_wide_compare(lower->base.mir, mir_op, a, b, result) &&
               anvil_lower_vreg_put(&lower->base.values, instr->result, result);
    }

    anvil_wide_target_t target = x86_wide_target(lower);
    bool is_unsigned = anvil_wide_value_unsigned(instr->result);
    bool ok;
    if (shift) {
        anvil_value_t *count = instr->operands[1];
        bool known = count->kind == ANVIL_VAL_CONST_INT;
        ok = anvil_wide_shift(lower->base.mir, &target, instr->op, a, b.lo, known, known ? count->data.i : 0, is_unsigned, &out);
    } else {
        ok = anvil_wide_binary(lower->base.mir, &target, instr->op, a, b, is_unsigned, &out);
    }
    return ok && anvil_lower_wide_put(&lower->pairs, instr->result, out);
}

static bool lower_instr(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->op == ANVIL_OP_NOP)
        return true;
    if (instr->op == ANVIL_OP_PHI)
        return true;

    /* Vector arithmetic and broadcasts, and the scalar operations the
     * target's tables price (FSQRT, MULH). */
    anvil_vector_mir_form_t form;
    if (anvil_vector_mir_form(lower->base.func->owner_ctx, instr, &form)) {
        anvil_mir_vreg_t uses[ANVIL_VECTOR_MAX_USES] = {ANVIL_MIR_NO_VREG, ANVIL_MIR_NO_VREG, ANVIL_MIR_NO_VREG};
        for (size_t operand = 0; operand < form.num_uses; operand++)
            uses[operand] = lower_value(lower, instr->operands[operand]);
        anvil_mir_vreg_t def = form.supported ? x86_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
        return anvil_vector_mir_emit(lower->base.mir, &form, def, uses) && anvil_lower_vreg_put(&lower->base.values, instr->result, def);
    }

    anvil_mir_opcode_t mir_op;
    if (instr->num_operands == 2 && anvil_ir_binary_op(instr->op, &mir_op)) {
        bool wide = (instr->operands[0] && x86_needs_pair(instr->operands[0]->type)) || (instr->operands[1] && x86_needs_pair(instr->operands[1]->type)) ||
                    (instr->result && x86_needs_pair(instr->result->type));
        if (wide)
            return lower_wide_binary(lower, instr, mir_op);

        anvil_mir_vreg_t lhs = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t rhs = lower_value(lower, instr->operands[1]);
        anvil_mir_vreg_t def = instr->result ? x86_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
        if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) {
            return false;
        }
        if (!anvil_ir_compare_op(mir_op) && !lower_match_binary_operand_sizes(lower, &lhs, &rhs, def)) {
            return false;
        }

        anvil_mir_vreg_t uses[] = {lhs, rhs};
        bool added = mir_op == ANVIL_MIR_OP_FCMP ? anvil_mir_add_instr_imm_uses(lower->base.mir, mir_op, def, uses, 2, instr->fcmp_pred) : anvil_mir_add_instr(lower->base.mir, mir_op, def, uses, 2);
        if (!added) {
            return false;
        }
        return anvil_lower_vreg_put(&lower->base.values, instr->result, def);
    }

    if (instr->num_operands == 1 && anvil_ir_unary_op(instr->op, &mir_op)) {
        if (instr->result && x86_needs_pair(instr->result->type)) {
            anvil_wide_pair_t a, out;
            bool is_unsigned = anvil_wide_value_unsigned(instr->result);
            if (!ensure_wide(lower, instr->operands[0], &a))
                return false;
            bool ok = instr->op == ANVIL_OP_NEG ? anvil_wide_neg(lower->base.mir, a, is_unsigned, &out)
                    : instr->op == ANVIL_OP_NOT ? anvil_wide_not(lower->base.mir, a, is_unsigned, &out) : false;
            return ok && anvil_lower_wide_put(&lower->pairs, instr->result, out);
        }
        anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t def = instr->result ? x86_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
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
            uses[operand] = lower_value(lower, instr->operands[operand]);
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
    case ANVIL_OP_ALLOCA:
        return lower_alloca(lower, instr);
    case ANVIL_OP_GEP:
        return lower_gep(lower, instr);
    case ANVIL_OP_STRUCT_GEP:
        return lower_struct_gep(lower, instr);
    case ANVIL_OP_SELECT:
        return lower_select(lower, instr);
    case ANVIL_OP_LOAD: {
        if (instr->num_operands != 1 || !instr->result)
            return false;
        if (x86_needs_pair(instr->result->type)) {
            return lower_load_i64_pair(lower, instr);
        }
        anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
        int64_t offset = 0;
        if (!lower_memory_address(lower, instr->operands[0], &ptr, &offset)) {
            return false;
        }
        anvil_mir_vreg_t def = x86_add_vreg_for_type(lower, instr->result->type);
        if (ptr == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG)
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
        if (!ok) {
            return false;
        }
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
        anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
        int64_t offset = 0;
        if (!lower_memory_address(lower, instr->operands[1], &ptr, &offset)) {
            return false;
        }
        if (ptr == ANVIL_MIR_NO_VREG)
            return false;
        if (instr->operands[0] && x86_needs_pair(instr->operands[0]->type)) {
            return lower_store_i64_pair(lower, instr->operands[0], ptr, offset);
        }
        anvil_mir_vreg_t val = lower_value(lower, instr->operands[0]);
        if (val == ANVIL_MIR_NO_VREG)
            return false;
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
            return add_return(lower, NULL);
        if (instr->num_operands == 1)
            return add_return(lower, instr->operands[0]);
        return false;
    default:
        return false;
    }
}

/* anvil_ir_lower_t hooks: a 64-bit integer in a word pair. */
static anvil_mir_vreg_t ir_value(anvil_ir_lower_t *base, anvil_value_t *value)
{
    return lower_value((x86_mir_lower_t *)base, value);
}

static bool ir_value_regs(anvil_ir_lower_t *base, anvil_value_t *value, anvil_mir_vreg_t regs[2], size_t *count)
{
    x86_mir_lower_t *lower = (x86_mir_lower_t *)base;
    if (x86_needs_pair(value->type)) {
        anvil_wide_pair_t pair;
        regs[0] = regs[1] = ANVIL_MIR_NO_VREG;
        *count = 2;
        if (!ensure_wide(lower, value, &pair))
            return false;
        regs[0] = pair.hi;
        regs[1] = pair.lo;
        return true;
    }
    regs[0] = lower_value(lower, value);
    *count = 1;
    return regs[0] != ANVIL_MIR_NO_VREG;
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
    lower.base.value = ir_value;
    lower.base.value_regs = ir_value_regs;
    lower.sret_pointer = ANVIL_MIR_NO_VREG;
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
        anvil_ir_lower_destroy(&lower.base);
        anvil_lower_map_destroy(&lower.pairs);
        anvil_lower_map_destroy(&lower.wide_consts);
        return NULL;
    }

    for (size_t rank = 0; rank < cfg.count; rank++) {
        anvil_block_t *block = cfg.blocks[cfg.layout[rank]];
        anvil_mir_block_t mir_block = anvil_lower_block_get(&lower.base.blocks, block);
        if (mir_block == ANVIL_MIR_NO_BLOCK || !anvil_mir_set_current_block(lower.base.mir, mir_block)) {
            anvil_opt_cfg_destroy(&cfg);
            anvil_mir_func_destroy(lower.base.mir);
            anvil_ir_lower_destroy(&lower.base);
            anvil_lower_map_destroy(&lower.pairs);
            anvil_lower_map_destroy(&lower.wide_consts);
            return NULL;
        }

        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            size_t first = anvil_mir_num_instrs(lower.base.mir);
            if (!lower_instr(&lower, instr) || !anvil_mir_annotate_memory(lower.base.mir, first, &instr->memory_access) ||
                (instr->op == ANVIL_OP_CALL && !anvil_mir_annotate_call_effects(lower.base.mir, first, anvil_instr_call_effects(instr)))) {
                anvil_opt_cfg_destroy(&cfg);
                anvil_mir_func_destroy(lower.base.mir);
                anvil_ir_lower_destroy(&lower.base);
                anvil_lower_map_destroy(&lower.pairs);
                anvil_lower_map_destroy(&lower.wide_consts);
                return NULL;
            }
        }
    }

    anvil_ir_lower_destroy(&lower.base);
    anvil_lower_map_destroy(&lower.pairs);
    anvil_lower_map_destroy(&lower.wide_consts);
    anvil_opt_cfg_destroy(&cfg);
    return lower.base.mir;
}
