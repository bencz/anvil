#include "../common/switch_lower.h"
#include "systemz_internal.h"
#include "../common/ir_lower.h"
#include "../common/lower_map.h"
#include "../common/wide_mir.h"
#include "../common/narrow_mir.h"

typedef struct {
    anvil_ir_lower_t base;
    const anvil_mainframe_target_desc_t *desc;

    /* The hidden SRET argument, returned when the ABI says so. */
    anvil_mir_vreg_t sret_pointer;
    /* Variadic functions: the first unnamed parameter-list entry. */
    anvil_mir_vreg_t va_base;
} systemz_lower_t;

static bool systemz_type_is_fp(anvil_type_t *type)
{
    return type && (type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64);
}

static bool systemz_type_is_void(anvil_type_t *type)
{
    return type && type->kind == ANVIL_TYPE_VOID;
}

static bool systemz_type_is_signed(anvil_type_t *type)
{
    if (!type)
        return false;
    return type->kind == ANVIL_TYPE_I1 || type->kind == ANVIL_TYPE_I8 || type->kind == ANVIL_TYPE_I16 || type->kind == ANVIL_TYPE_I32 || type->kind == ANVIL_TYPE_I64;
}

static bool systemz_type_is_64bit_integer(anvil_type_t *type)
{
    return type && (type->kind == ANVIL_TYPE_I64 || type->kind == ANVIL_TYPE_U64);
}

static anvil_mir_reg_class_t systemz_reg_class_for_type(anvil_type_t *type)
{
    return systemz_type_is_fp(type) ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR;
}

static uint16_t systemz_bits_for_type(const anvil_mainframe_target_desc_t *desc, anvil_type_t *type)
{
    if (!type)
        return (uint16_t)(desc->word_size * 8);
    if (type->kind == ANVIL_TYPE_PTR || type->kind == ANVIL_TYPE_FUNC) {
        return (uint16_t)(desc->ptr_size * 8);
    }
    if (type->kind == ANVIL_TYPE_VOID)
        return 0;
    if (type->size == 0)
        return (uint16_t)(desc->word_size * 8);
    return (uint16_t)(type->size * 8);
}

static anvil_mir_vreg_t systemz_add_vreg_for_type(systemz_lower_t *lower, anvil_type_t *type)
{
    return anvil_mir_add_vreg_typed(lower->base.mir, systemz_reg_class_for_type(type), systemz_bits_for_type(lower->desc, type), systemz_type_is_signed(type));
}

/* 64-bit integers on targets whose GPRs hold 32 bits travel as word pairs
 * (common/wide_mir); the parameter-list and result conventions for them
 * come from the ABI (systemz_abi_ops_t). */
static bool systemz_needs_pair(const systemz_lower_t *lower, anvil_type_t *type)
{
    return !lower->desc->has_64bit_gprs && systemz_type_is_64bit_integer(type);
}

static bool systemz_type_is_u64(anvil_type_t *type)
{
    return type && type->kind == ANVIL_TYPE_U64;
}

static bool systemz_lower_params(systemz_lower_t *lower)
{
    int64_t entry = 0;
    for (size_t i = 0; i < lower->base.func->num_params; i++) {
        anvil_value_t *param = lower->base.func->params[i];
        if (systemz_needs_pair(lower, param->type)) {
            /* Two consecutive entries, high word first. */
            anvil_wide_pair_t pair;
            if (!anvil_wide_new(lower->base.mir, systemz_type_is_u64(param->type), &pair) ||
                !anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, pair.hi, entry * (int64_t)lower->desc->ptr_size) ||
                !anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, pair.lo, (entry + 1) * (int64_t)lower->desc->ptr_size) ||
                !anvil_lower_wide_put(&lower->base.pairs, param, pair))
                return false;
            entry += 2;
            continue;
        }
        anvil_mir_vreg_t local = systemz_add_vreg_for_type(lower, param->type);
        if (local == ANVIL_MIR_NO_VREG)
            return false;
        int64_t slot = entry++ * (int64_t)lower->desc->ptr_size;
        if (!anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, local, slot)) {
            return false;
        }
        /* A BYVAL parameter is already the address of the callee's copy. */
        if (anvil_type_func_param_attr(lower->base.func->type, i) == ANVIL_PARAM_SRET)
            lower->sret_pointer = local;
        if (!anvil_lower_vreg_put(&lower->base.values, param, local))
            return false;
    }

    /* R1 addresses the list on entry only (calls reload it): a variadic
     * function keeps the address of its first unnamed entry. */
    if (lower->base.func->type->data.func.variadic) {
        uint16_t bits = (uint16_t)(lower->desc->ptr_size * 8);
        anvil_mir_vreg_t list = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, bits, false);
        anvil_mir_vreg_t base = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, bits, false);
        anvil_mir_vreg_t offset = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, bits, false);
        anvil_mir_vreg_t copy = anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, bits, false);
        anvil_mir_vreg_t list_use[] = {list}, add_uses[] = {copy, offset};
        if (list == ANVIL_MIR_NO_VREG || base == ANVIL_MIR_NO_VREG || offset == ANVIL_MIR_NO_VREG || copy == ANVIL_MIR_NO_VREG ||
            !anvil_mir_set_fixed_reg(lower->base.mir, list, 1) || !anvil_mir_set_live_in(lower->base.mir, list, true) ||
            !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, copy, list_use, 1) ||
            !anvil_mir_add_instr_imm(lower->base.mir, ANVIL_MIR_OP_MOV, offset, entry * (int64_t)lower->desc->ptr_size) ||
            !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_ADD, base, add_uses, 2))
            return false;
        lower->va_base = base;
    }
    return true;
}

static bool systemz_add_return_copy(systemz_lower_t *lower, anvil_value_t *value)
{
    if (!value && lower->sret_pointer != ANVIL_MIR_NO_VREG && lower->desc->abi_ops->sret_returns_address) {
        const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(lower->base.mir, lower->sret_pointer);
        anvil_mir_vreg_t ret = info ? anvil_mir_add_vreg_typed(lower->base.mir, ANVIL_MIR_REG_GPR, info->size_bits, false) : ANVIL_MIR_NO_VREG;
        anvil_mir_vreg_t uses[] = {lower->sret_pointer};
        if (ret == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->base.mir, ret, lower->desc->return_gpr) ||
            !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, ret, uses, 1))
            return false;
        uses[0] = ret;
        return anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, uses, 1);
    }
    if (!value) {
        return anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, NULL, 0);
    }
    if (systemz_needs_pair(lower, value->type)) {
        /* The emitter places the words in the ABI's result GPR pair. */
        anvil_wide_pair_t pair;
        if (lower->desc->abi_ops->pair_low_gpr < 0 || !anvil_ir_value_pair(&lower->base, value, &pair))
            return false;
        anvil_mir_vreg_t lo[] = {pair.lo}, hi[] = {pair.hi};
        return anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_RET_VALUE_PART, ANVIL_MIR_NO_VREG, lo, 1) &&
               anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, hi, 1);
    }

    anvil_mir_vreg_t src = anvil_ir_lower_value(&lower->base, value);
    if (src == ANVIL_MIR_NO_VREG)
        return false;

    anvil_mir_vreg_t ret = systemz_add_vreg_for_type(lower, value->type);
    if (ret == ANVIL_MIR_NO_VREG)
        return false;
    if (!anvil_mir_set_fixed_reg(lower->base.mir, ret, systemz_type_is_fp(value->type) ? lower->desc->return_fpr : lower->desc->return_gpr)) {
        return false;
    }

    anvil_mir_vreg_t uses[] = {src};
    if (!anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, ret, uses, 1)) {
        return false;
    }
    uses[0] = ret;
    return anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, uses, 1);
}

/* Appends a call's parameter-list bundle: one entry per argument, two
 * (high word first) for a word pair. */
static bool systemz_add_call_args(systemz_lower_t *lower, const anvil_mir_vreg_t *entries, size_t num_entries)
{
    for (size_t i = 0; i < num_entries; i++) {
        anvil_mir_vreg_t uses[] = {entries[i]};
        if (!anvil_mir_add_instr_imm_uses(lower->base.mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, uses, 1, (int64_t)i))
            return false;
    }
    return true;
}

/* CALL plus the copies out of the result registers.  A pair result arrives
 * in the result GPR (high word) and the ABI's pair_low_gpr. */
static bool systemz_add_call(systemz_lower_t *lower, const char *symbol, anvil_mir_vreg_t callee, anvil_mir_reg_class_t result_class,
                             uint16_t result_bits, bool result_signed, anvil_wide_pair_t result)
{
    anvil_mir_vreg_t call_def = ANVIL_MIR_NO_VREG;
    if (result.lo != ANVIL_MIR_NO_VREG) {
        bool pair = result.hi != ANVIL_MIR_NO_VREG;
        if (pair && lower->desc->abi_ops->pair_low_gpr < 0)
            return false;
        call_def = anvil_mir_add_vreg_typed(lower->base.mir, result_class, pair ? 32 : result_bits, result_signed);
        if (call_def == ANVIL_MIR_NO_VREG ||
            !anvil_mir_set_fixed_reg(lower->base.mir, call_def, result_class == ANVIL_MIR_REG_FPR ? lower->desc->return_fpr : lower->desc->return_gpr))
            return false;
    }
    anvil_mir_vreg_t uses[] = {callee};
    if (!anvil_mir_add_call(lower->base.mir, call_def, callee != ANVIL_MIR_NO_VREG ? uses : NULL, callee != ANVIL_MIR_NO_VREG ? 1 : 0, symbol, ANVIL_CC_MVS, false, 0))
        return false;
    /* The linkage preserves GPRs (STM/LM) but no FPRs. */
    if (!anvil_mir_set_instr_clobbers(lower->base.mir, anvil_mir_num_instrs(lower->base.mir) - 1, ANVIL_MIR_REG_FPR, UINT64_C(0xffff)))
        return false;
    if (call_def == ANVIL_MIR_NO_VREG)
        return true;
    if (result.hi != ANVIL_MIR_NO_VREG && !anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_CALL_RESULT, result.lo, NULL, 0))
        return false;
    anvil_mir_vreg_t copy_uses[] = {call_def};
    return anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, result.hi != ANVIL_MIR_NO_VREG ? result.hi : result.lo, copy_uses, 1);
}

/* 64-bit run-time helpers (libgcc names) in the module's own linkage. */
static bool systemz_wide_call_helper(void *ctx, const char *name, const anvil_wide_pair_t *args, size_t num_args, anvil_wide_pair_t result)
{
    systemz_lower_t *lower = ctx;
    anvil_mir_vreg_t entries[8];
    size_t count = 0;
    for (size_t i = 0; i < num_args; i++) {
        if (count + 2 > sizeof(entries) / sizeof(entries[0]))
            return false;
        if (args[i].hi != ANVIL_MIR_NO_VREG)
            entries[count++] = args[i].hi;
        entries[count++] = args[i].lo;
    }
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(lower->base.mir, result.lo);
    return info && systemz_add_call_args(lower, entries, count) &&
           systemz_add_call(lower, name, ANVIL_MIR_NO_VREG, info->reg_class, info->size_bits, info->is_signed, result);
}

static bool systemz_lower_call(systemz_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands == 0)
        return false;
    if (instr->call_cc != ANVIL_CC_MVS)
        return false;

    size_t num_args = instr->num_operands - 1;
    anvil_mir_vreg_t *entries = num_args ? calloc(num_args * 2, sizeof(*entries)) : NULL;
    if (num_args && !entries)
        return false;
    size_t num_entries = 0;
    /* Materializing constants/symbols can itself append MIR. Resolve every
       value before starting the contiguous CALL_STACK_ARG bundle. */
    anvil_value_t *callee_value = instr->operands[0];
    anvil_type_t *fn_type = callee_value->type && callee_value->type->kind == ANVIL_TYPE_PTR
        ? callee_value->type->data.pointee : callee_value->type;
    anvil_agg_target_t target = {lower->desc->ptr_size, true};
    bool ok = true;
    for (size_t i = 0; ok && i < num_args; i++) {
        anvil_value_t *arg = instr->operands[i + 1];
        if (systemz_needs_pair(lower, arg->type)) {
            anvil_wide_pair_t pair;
            ok = anvil_ir_value_pair(&lower->base, arg, &pair);
            entries[num_entries++] = pair.hi;
            entries[num_entries++] = pair.lo;
            continue;
        }
        anvil_mir_vreg_t value = anvil_ir_lower_value(&lower->base, arg);
        ok = value != ANVIL_MIR_NO_VREG;
        if (ok && anvil_call_arg_attr(instr, fn_type, i) == ANVIL_PARAM_BYVAL) {
            /* The slot carries the address of a copy made for this call. */
            anvil_type_t *object = arg->type->data.pointee;
            anvil_mir_vreg_t copy = anvil_agg_frame_object(lower->base.mir, object->size, object->align, target);
            ok = copy != ANVIL_MIR_NO_VREG && anvil_agg_copy(lower->base.mir, value, copy, object->size, target);
            value = copy;
        }
        entries[num_entries++] = value;
    }
    anvil_mir_vreg_t callee = ANVIL_MIR_NO_VREG;
    if (ok && callee_value->kind != ANVIL_VAL_FUNC) {
        callee = anvil_ir_lower_value(&lower->base, callee_value);
        ok = callee != ANVIL_MIR_NO_VREG;
    }
    ok = ok && systemz_add_call_args(lower, entries, num_entries);
    free(entries);
    if (!ok)
        return false;

    anvil_type_t *result_type = instr->result ? instr->result->type : NULL;
    anvil_wide_pair_t result = {ANVIL_MIR_NO_VREG, ANVIL_MIR_NO_VREG};
    if (result_type && !systemz_type_is_void(result_type)) {
        if (systemz_needs_pair(lower, result_type))
            ok = anvil_wide_new(lower->base.mir, systemz_type_is_u64(result_type), &result);
        else
            ok = (result.lo = systemz_add_vreg_for_type(lower, result_type)) != ANVIL_MIR_NO_VREG;
    }
    const char *symbol = callee_value->kind == ANVIL_VAL_FUNC ? callee_value->name : NULL;
    if (!ok || !systemz_add_call(lower, symbol, callee, systemz_reg_class_for_type(result_type), systemz_bits_for_type(lower->desc, result_type),
                                 systemz_type_is_signed(result_type), result))
        return false;
    if (result.lo == ANVIL_MIR_NO_VREG)
        return true;
    return result.hi != ANVIL_MIR_NO_VREG ? anvil_lower_wide_put(&lower->base.pairs, instr->result, result) : anvil_lower_vreg_put(&lower->base.values, instr->result, result.lo);
}

static bool systemz_target_instr(anvil_ir_lower_t *base, anvil_instr_t *instr)
{
    systemz_lower_t *lower = (systemz_lower_t *)base;
    if (instr->op == ANVIL_OP_VA_START) {
        if (!instr->result || lower->va_base == ANVIL_MIR_NO_VREG)
            return false;
        anvil_mir_vreg_t def = systemz_add_vreg_for_type(lower, instr->result->type);
        anvil_mir_vreg_t uses[] = {lower->va_base};
        return def != ANVIL_MIR_NO_VREG && anvil_mir_add_instr(lower->base.mir, ANVIL_MIR_OP_COPY, def, uses, 1) && anvil_lower_vreg_put(&lower->base.values, instr->result, def);
    }

    switch (instr->op) {
    case ANVIL_OP_CALL:
        return systemz_lower_call(lower, instr);

    case ANVIL_OP_RET:
        return systemz_add_return_copy(lower, instr->num_operands ? instr->operands[0] : NULL);

    default:
        return false;
    }
}

static anvil_mir_vreg_t systemz_ir_vreg_for_type(anvil_ir_lower_t *base, anvil_type_t *type)
{
    return systemz_add_vreg_for_type((systemz_lower_t *)base, type);
}

static bool systemz_ir_lower_params(anvil_ir_lower_t *base)
{
    return systemz_lower_params((systemz_lower_t *)base);
}

static bool systemz_ir_displacement_fits(const anvil_ir_lower_t *base, int64_t displacement)
{
    return systemz_displacement_fits(displacement, anvil_mir_func_cpu_features(base->mir));
}

static bool systemz_ir_pair_type(const anvil_ir_lower_t *base, anvil_type_t *type)
{
    return systemz_needs_pair((const systemz_lower_t *)base, type);
}

anvil_mir_func_t *anvil_mainframe_lower_func_to_mir(anvil_func_t *func, anvil_mainframe_variant_t variant)
{
    const anvil_mainframe_target_desc_t *desc = anvil_mainframe_get_target_desc(variant);
    if (!func || !desc || !func->entry || !func->type || func->type->kind != ANVIL_TYPE_FUNC || func->type->data.func.cc != ANVIL_CC_MVS)
        return NULL;

    systemz_lower_t lower;
    memset(&lower, 0, sizeof(lower));
    lower.desc = desc;
    lower.base.func = func;
    lower.base.vreg_for_type = systemz_ir_vreg_for_type;
    lower.base.lower_params = systemz_ir_lower_params;
    lower.base.target_instr = systemz_target_instr;
    lower.base.narrowest_operation_bits = 32;
    lower.base.pointer_bits = (unsigned)(desc->ptr_size * 8);
    lower.base.stack_alignment = desc->stack_alignment;
    lower.base.displacement_fits = systemz_ir_displacement_fits;
    lower.base.pair_type = systemz_ir_pair_type;
    lower.base.wide = (anvil_wide_target_t){systemz_wide_call_helper, &lower};
    lower.base.select_classes = ANVIL_IR_SELECT_EVERY_CLASS;
    lower.sret_pointer = ANVIL_MIR_NO_VREG;
    lower.va_base = ANVIL_MIR_NO_VREG;

    anvil_mir_func_t *mir = anvil_ir_lower_function(&lower.base);
    char verify_error[256] = {0};
    bool verified = mir && anvil_mir_verify(mir, verify_error, sizeof(verify_error));
    if (!verified) {
        if (func->parent && func->parent->ctx) {
            anvil_set_error(func->parent->ctx, ANVIL_ERR_CODEGEN, "Mainframe MIR lowering failed: %s", mir ? verify_error : "unsupported IR operation");
        }
        anvil_mir_func_destroy(mir);
        return NULL;
    }
    return mir;
}
