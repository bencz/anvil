/*
 * ANVIL - ARM64 lowering to target-independent MachineIR.
 *
 * ARM64 is the current reference backend for the shared MachineIR/regalloc
 * path. This file lowers source IR into MachineIR, models ABI register and
 * stack constraints, runs allocation/spill materialization, and emits ARM64
 * assembly from the allocated machine instructions.
 */

#include "anvil/anvil_arm64_mir.h"
#include "anvil/anvil_internal.h"
#include "anvil/anvil_analysis.h"
#include "arm64_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    anvil_value_t *value;
    anvil_mir_vreg_t vreg;
} value_vreg_t;

typedef struct {
    anvil_value_t *value;
    anvil_mir_vreg_t base;
    int64_t offset;
} value_addr_offset_t;

typedef struct {
    anvil_block_t *block;
    anvil_mir_block_t mir_block;
} block_map_t;

typedef anvil_mir_parallel_copy_t phi_edge_copy_t;

typedef struct {
    anvil_block_t *dest_block;
    anvil_mir_block_t edge_block;
    anvil_mir_block_t dest_mir_block;
} pending_phi_edge_t;

typedef struct {
    anvil_func_t *func;
    anvil_mir_func_t *mir;
    value_vreg_t *values;
    size_t num_values;
    size_t cap_values;
    value_addr_offset_t *addr_offsets;
    size_t num_addr_offsets;
    size_t cap_addr_offsets;
    block_map_t *blocks;
    size_t num_blocks;
    size_t num_edge_blocks;
    bool failed;
} arm64_mir_lower_t;

static bool arm64_type_is_fp(anvil_type_t *type)
{
    return type && (type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64);
}

static bool arm64_type_is_void(anvil_type_t *type)
{
    return !type || type->kind == ANVIL_TYPE_VOID;
}

static bool arm64_type_is_signed(anvil_type_t *type)
{
    return type && type->is_signed && !arm64_type_is_fp(type);
}

static anvil_mir_reg_class_t arm64_reg_class_for_type(anvil_type_t *type)
{
    return arm64_type_is_fp(type) ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR;
}

static uint16_t arm64_bits_for_type(anvil_type_t *type)
{
    if (!type) return 64;
    if (type->kind == ANVIL_TYPE_PTR) return 64;

    size_t size = anvil_type_size(type);
    if (size == 0) return 64;
    if (size > UINT16_MAX / 8) return 64;
    return (uint16_t)(size * 8);
}

static uint16_t arm64_slot_bits_for_type(anvil_type_t *type)
{
    size_t size = type ? anvil_type_size(type) : 8;
    if (size == 0) size = 8;
    if (size > UINT16_MAX / 8) size = UINT16_MAX / 8;
    return (uint16_t)(size * 8);
}

static uint16_t arm64_align_for_type(anvil_type_t *type)
{
    int align = type ? arm64_type_align(type) : 8;
    if (align <= 0) align = 8;
    if (align > UINT16_MAX) align = UINT16_MAX;
    return (uint16_t)align;
}

static anvil_mir_vreg_t arm64_add_vreg_for_type(arm64_mir_lower_t *lower,
                                                anvil_type_t *type)
{
    return anvil_mir_add_vreg_typed(lower->mir,
                                    arm64_reg_class_for_type(type),
                                    arm64_bits_for_type(type),
                                    arm64_type_is_signed(type));
}

static bool map_reserve(arm64_mir_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_values) return true;

    size_t new_cap = lower->cap_values ? lower->cap_values * 2 : 32;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) return false;
        new_cap *= 2;
    }

    value_vreg_t *grown = realloc(lower->values, new_cap * sizeof(*grown));
    if (!grown) return false;

    lower->values = grown;
    lower->cap_values = new_cap;
    return true;
}

static bool map_put(arm64_mir_lower_t *lower, anvil_value_t *value,
                    anvil_mir_vreg_t vreg)
{
    if (!value || vreg == ANVIL_MIR_NO_VREG) return false;

    for (size_t i = 0; i < lower->num_values; i++) {
        if (lower->values[i].value == value) {
            lower->values[i].vreg = vreg;
            return true;
        }
    }

    if (!map_reserve(lower, lower->num_values + 1)) return false;
    lower->values[lower->num_values].value = value;
    lower->values[lower->num_values].vreg = vreg;
    lower->num_values++;
    return true;
}

static anvil_mir_vreg_t map_get(arm64_mir_lower_t *lower, anvil_value_t *value)
{
    for (size_t i = 0; i < lower->num_values; i++) {
        if (lower->values[i].value == value) return lower->values[i].vreg;
    }
    return ANVIL_MIR_NO_VREG;
}

static bool addr_map_reserve(arm64_mir_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_addr_offsets) return true;

    size_t new_cap = lower->cap_addr_offsets ? lower->cap_addr_offsets * 2 : 16;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) return false;
        new_cap *= 2;
    }

    value_addr_offset_t *grown =
        realloc(lower->addr_offsets, new_cap * sizeof(*grown));
    if (!grown) return false;

    lower->addr_offsets = grown;
    lower->cap_addr_offsets = new_cap;
    return true;
}

static bool addr_map_put(arm64_mir_lower_t *lower,
                         anvil_value_t *value,
                         anvil_mir_vreg_t base,
                         int64_t offset)
{
    if (!value || base == ANVIL_MIR_NO_VREG) return false;

    for (size_t i = 0; i < lower->num_addr_offsets; i++) {
        if (lower->addr_offsets[i].value == value) {
            lower->addr_offsets[i].base = base;
            lower->addr_offsets[i].offset = offset;
            return true;
        }
    }

    if (!addr_map_reserve(lower, lower->num_addr_offsets + 1)) return false;
    lower->addr_offsets[lower->num_addr_offsets].value = value;
    lower->addr_offsets[lower->num_addr_offsets].base = base;
    lower->addr_offsets[lower->num_addr_offsets].offset = offset;
    lower->num_addr_offsets++;
    return true;
}

static bool addr_map_get(arm64_mir_lower_t *lower,
                         anvil_value_t *value,
                         anvil_mir_vreg_t *out_base,
                         int64_t *out_offset)
{
    if (!value) return false;
    for (size_t i = 0; i < lower->num_addr_offsets; i++) {
        if (lower->addr_offsets[i].value == value) {
            if (out_base) *out_base = lower->addr_offsets[i].base;
            if (out_offset) *out_offset = lower->addr_offsets[i].offset;
            return true;
        }
    }
    return false;
}

static anvil_mir_block_t block_get(arm64_mir_lower_t *lower,
                                   anvil_block_t *block)
{
    if (!block) return ANVIL_MIR_NO_BLOCK;
    for (size_t i = 0; i < lower->num_blocks; i++) {
        if (lower->blocks[i].block == block) return lower->blocks[i].mir_block;
    }
    return ANVIL_MIR_NO_BLOCK;
}

static bool create_mir_blocks(arm64_mir_lower_t *lower)
{
    lower->num_blocks = lower->func->num_blocks;
    if (lower->num_blocks == 0) return true;

    lower->blocks = calloc(lower->num_blocks, sizeof(*lower->blocks));
    if (!lower->blocks) return false;

    size_t index = 0;
    for (anvil_block_t *block = lower->func->blocks; block; block = block->next) {
        anvil_mir_block_t mir_block;
        if (index == 0) {
            mir_block = anvil_mir_current_block(lower->mir);
        } else {
            mir_block = anvil_mir_add_block(lower->mir, block->name);
        }
        if (mir_block == ANVIL_MIR_NO_BLOCK) return false;

        lower->blocks[index].block = block;
        lower->blocks[index].mir_block = mir_block;
        index++;
    }

    return true;
}

static bool set_fixed_register_arg(arm64_mir_lower_t *lower,
                                   anvil_mir_vreg_t vreg,
                                   anvil_type_t *type,
                                   size_t *gpr_count,
                                   size_t *fpr_count)
{
    if (arm64_type_is_fp(type)) {
        if (*fpr_count >= 8) return false;
        if (!anvil_mir_set_fixed_reg(lower->mir, vreg, (int)*fpr_count)) {
            return false;
        }
        (*fpr_count)++;
        return true;
    }

    if (*gpr_count >= 8) return false;
    if (!anvil_mir_set_fixed_reg(lower->mir, vreg, (int)*gpr_count)) {
        return false;
    }
    (*gpr_count)++;
    return true;
}

static bool arg_still_uses_register(anvil_type_t *type,
                                    size_t gpr_count,
                                    size_t fpr_count)
{
    return arm64_type_is_fp(type) ? fpr_count < 8 : gpr_count < 8;
}

static void advance_arg_count(anvil_type_t *type,
                              size_t *gpr_count,
                              size_t *fpr_count)
{
    if (arm64_type_is_fp(type)) {
        (*fpr_count)++;
    } else {
        (*gpr_count)++;
    }
}

static int64_t arm64_stack_arg_slot_size(anvil_type_t *type)
{
    int64_t size = type ? arm64_type_size(type) : 8;
    if (size <= 0) size = 8;
    if (size < 8) size = 8;
    return (size + 7) & ~INT64_C(7);
}

static anvil_abi_t lower_abi(const arm64_mir_lower_t *lower)
{
    if (!lower || !lower->func || !lower->func->parent ||
        !lower->func->parent->ctx) {
        return ANVIL_ABI_DEFAULT;
    }
    return lower->func->parent->ctx->abi;
}

static bool lower_params(arm64_mir_lower_t *lower)
{
    size_t gpr_count = 0;
    size_t fpr_count = 0;
    int64_t stack_offset = 0;

    anvil_mir_block_t entry = block_get(lower, lower->func->blocks);
    if (entry == ANVIL_MIR_NO_BLOCK ||
        !anvil_mir_set_current_block(lower->mir, entry)) {
        return false;
    }

    for (size_t i = 0; i < lower->func->num_params; i++) {
        anvil_value_t *param = lower->func->params[i];
        if (!param) return false;

        anvil_mir_vreg_t local = arm64_add_vreg_for_type(lower, param->type);
        if (local == ANVIL_MIR_NO_VREG) {
            return false;
        }

        if (arg_still_uses_register(param->type, gpr_count, fpr_count)) {
            anvil_mir_vreg_t incoming =
                arm64_add_vreg_for_type(lower, param->type);
            if (incoming == ANVIL_MIR_NO_VREG) return false;
            if (!set_fixed_register_arg(lower, incoming, param->type,
                                        &gpr_count, &fpr_count) ||
                !anvil_mir_set_live_in(lower->mir, incoming, true)) {
                return false;
            }

            anvil_mir_vreg_t uses[] = { incoming };
            if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                                     local, uses, 1)) {
                return false;
            }
        } else {
            if (!anvil_mir_add_instr_imm(lower->mir,
                                         ANVIL_MIR_OP_INCOMING_STACK_ARG,
                                         local, stack_offset)) {
                return false;
            }
            stack_offset += arm64_stack_arg_slot_size(param->type);
            advance_arg_count(param->type, &gpr_count, &fpr_count);
        }

        if (!map_put(lower, param, local)) return false;
    }

    return true;
}

static bool prepare_phi_results(arm64_mir_lower_t *lower)
{
    for (anvil_block_t *block = lower->func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_PHI) break;
            if (!instr->result) return false;

            anvil_mir_vreg_t vreg = arm64_add_vreg_for_type(lower,
                                                            instr->result->type);
            if (vreg == ANVIL_MIR_NO_VREG) return false;
            if (!map_put(lower, instr->result, vreg)) return false;
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

static anvil_mir_vreg_t lower_value(arm64_mir_lower_t *lower,
                                    anvil_value_t *value);
static bool lower_add_const_offset(arm64_mir_lower_t *lower,
                                   anvil_mir_vreg_t base,
                                   int64_t offset,
                                   anvil_mir_vreg_t *out_ptr);

static anvil_mir_vreg_t lower_const_value(arm64_mir_lower_t *lower,
                                          anvil_value_t *value)
{
    anvil_mir_vreg_t vreg = arm64_add_vreg_for_type(lower, value->type);
    if (vreg == ANVIL_MIR_NO_VREG) return ANVIL_MIR_NO_VREG;

    int64_t imm = 0;
    switch (value->kind) {
        case ANVIL_VAL_CONST_INT:
            imm = value->data.i;
            break;
        case ANVIL_VAL_CONST_NULL:
            imm = 0;
            break;
        case ANVIL_VAL_CONST_FLOAT:
            imm = float_bits_as_i64(value->data.f, arm64_bits_for_type(value->type));
            break;
        default:
            return ANVIL_MIR_NO_VREG;
    }

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, vreg, imm)) {
        return ANVIL_MIR_NO_VREG;
    }
    return vreg;
}

static anvil_mir_vreg_t lower_symbol_address(arm64_mir_lower_t *lower,
                                             const char *symbol)
{
    if (!symbol || !symbol[0]) return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t vreg =
        anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 64, false);
    if (vreg == ANVIL_MIR_NO_VREG) return ANVIL_MIR_NO_VREG;

    if (!anvil_mir_add_instr_symbol(lower->mir, ANVIL_MIR_OP_SYMBOL_ADDR,
                                    vreg, NULL, 0, symbol)) {
        return ANVIL_MIR_NO_VREG;
    }
    return vreg;
}

static anvil_mir_vreg_t lower_reloc_address(arm64_mir_lower_t *lower,
                                             anvil_value_t *value)
{
    const char *symbol = value && value->data.reloc.symbol
                             ? value->data.reloc.symbol->name : NULL;
    if (!symbol || !symbol[0]) return ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t vreg = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_GPR, 64, false);
    if (vreg == ANVIL_MIR_NO_VREG ||
        !anvil_mir_add_instr_symbol_imm(
            lower->mir, ANVIL_MIR_OP_SYMBOL_ADDR, vreg, NULL, 0, symbol,
            value->data.reloc.addend)) return ANVIL_MIR_NO_VREG;
    return vreg;
}

static anvil_mir_vreg_t lower_string_address(arm64_mir_lower_t *lower,
                                             anvil_value_t *value)
{
    const char *label = NULL;
    if (anvil_mir_add_string_literal(lower->mir, value->data.str,
                                     &label) < 0 || !label) {
        return ANVIL_MIR_NO_VREG;
    }
    return lower_symbol_address(lower, label);
}

static anvil_mir_vreg_t lower_value(arm64_mir_lower_t *lower,
                                    anvil_value_t *value)
{
    if (!value) return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t existing = map_get(lower, value);
    if (existing != ANVIL_MIR_NO_VREG) return existing;

    anvil_mir_vreg_t base = ANVIL_MIR_NO_VREG;
    int64_t offset = 0;
    if (addr_map_get(lower, value, &base, &offset)) {
        anvil_mir_vreg_t materialized = ANVIL_MIR_NO_VREG;
        if (!lower_add_const_offset(lower, base, offset, &materialized)) {
            return ANVIL_MIR_NO_VREG;
        }
        if (!map_put(lower, value, materialized)) return ANVIL_MIR_NO_VREG;
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
                return lower_symbol_address(lower, value->data.func->name);
            }
            return lower_symbol_address(lower, value->name);
        case ANVIL_VAL_GLOBAL:
            return lower_symbol_address(lower, value->name);
        default:
            return ANVIL_MIR_NO_VREG;
    }
}

static bool map_binop(anvil_op_t op, anvil_mir_opcode_t *out_op)
{
    switch (op) {
        case ANVIL_OP_ADD:
        case ANVIL_OP_FADD:
            *out_op = ANVIL_MIR_OP_ADD;
            return true;
        case ANVIL_OP_SUB:
        case ANVIL_OP_FSUB:
            *out_op = ANVIL_MIR_OP_SUB;
            return true;
        case ANVIL_OP_MUL:
        case ANVIL_OP_FMUL:
            *out_op = ANVIL_MIR_OP_MUL;
            return true;
        case ANVIL_OP_SDIV:
            *out_op = ANVIL_MIR_OP_SDIV;
            return true;
        case ANVIL_OP_UDIV:
            *out_op = ANVIL_MIR_OP_UDIV;
            return true;
        case ANVIL_OP_FDIV:
            *out_op = ANVIL_MIR_OP_FDIV;
            return true;
        case ANVIL_OP_SMOD:
            *out_op = ANVIL_MIR_OP_SMOD;
            return true;
        case ANVIL_OP_UMOD:
            *out_op = ANVIL_MIR_OP_UMOD;
            return true;
        case ANVIL_OP_AND:
            *out_op = ANVIL_MIR_OP_AND;
            return true;
        case ANVIL_OP_OR:
            *out_op = ANVIL_MIR_OP_OR;
            return true;
        case ANVIL_OP_XOR:
            *out_op = ANVIL_MIR_OP_XOR;
            return true;
        case ANVIL_OP_SHL:
            *out_op = ANVIL_MIR_OP_SHL;
            return true;
        case ANVIL_OP_SHR:
            *out_op = ANVIL_MIR_OP_SHR;
            return true;
        case ANVIL_OP_SAR:
            *out_op = ANVIL_MIR_OP_SAR;
            return true;
        case ANVIL_OP_CMP_EQ:
            *out_op = ANVIL_MIR_OP_CMP_EQ;
            return true;
        case ANVIL_OP_CMP_NE:
            *out_op = ANVIL_MIR_OP_CMP_NE;
            return true;
        case ANVIL_OP_CMP_LT:
            *out_op = ANVIL_MIR_OP_CMP_LT;
            return true;
        case ANVIL_OP_CMP_LE:
            *out_op = ANVIL_MIR_OP_CMP_LE;
            return true;
        case ANVIL_OP_CMP_GT:
            *out_op = ANVIL_MIR_OP_CMP_GT;
            return true;
        case ANVIL_OP_CMP_GE:
            *out_op = ANVIL_MIR_OP_CMP_GE;
            return true;
        case ANVIL_OP_CMP_ULT:
            *out_op = ANVIL_MIR_OP_CMP_ULT;
            return true;
        case ANVIL_OP_CMP_ULE:
            *out_op = ANVIL_MIR_OP_CMP_ULE;
            return true;
        case ANVIL_OP_CMP_UGT:
            *out_op = ANVIL_MIR_OP_CMP_UGT;
            return true;
        case ANVIL_OP_CMP_UGE:
            *out_op = ANVIL_MIR_OP_CMP_UGE;
            return true;
        case ANVIL_OP_FCMP:
            *out_op = ANVIL_MIR_OP_FCMP;
            return true;
        default:
            return false;
    }
}

static bool mir_op_is_compare(anvil_mir_opcode_t op)
{
    switch (op) {
        case ANVIL_MIR_OP_CMP:
        case ANVIL_MIR_OP_FCMP:
        case ANVIL_MIR_OP_CMP_EQ:
        case ANVIL_MIR_OP_CMP_NE:
        case ANVIL_MIR_OP_CMP_LT:
        case ANVIL_MIR_OP_CMP_LE:
        case ANVIL_MIR_OP_CMP_GT:
        case ANVIL_MIR_OP_CMP_GE:
        case ANVIL_MIR_OP_CMP_ULT:
        case ANVIL_MIR_OP_CMP_ULE:
        case ANVIL_MIR_OP_CMP_UGT:
        case ANVIL_MIR_OP_CMP_UGE:
            return true;
        default:
            return false;
    }
}

static bool map_unop(anvil_op_t op, anvil_mir_opcode_t *out_op)
{
    switch (op) {
        case ANVIL_OP_NEG:
        case ANVIL_OP_FNEG:
            *out_op = ANVIL_MIR_OP_NEG;
            return true;
        case ANVIL_OP_NOT:
            *out_op = ANVIL_MIR_OP_NOT;
            return true;
        case ANVIL_OP_FABS:
            *out_op = ANVIL_MIR_OP_FABS;
            return true;
        default:
            return false;
    }
}

static bool map_cast(anvil_op_t op, anvil_mir_opcode_t *out_op)
{
    switch (op) {
        case ANVIL_OP_ZEXT:
            *out_op = ANVIL_MIR_OP_ZEXT;
            return true;
        case ANVIL_OP_SEXT:
            *out_op = ANVIL_MIR_OP_SEXT;
            return true;
        case ANVIL_OP_TRUNC:
            *out_op = ANVIL_MIR_OP_TRUNC;
            return true;
        case ANVIL_OP_BITCAST:
        case ANVIL_OP_PTRTOINT:
        case ANVIL_OP_INTTOPTR:
            *out_op = ANVIL_MIR_OP_BITCAST;
            return true;
        case ANVIL_OP_SITOFP:
            *out_op = ANVIL_MIR_OP_SITOFP;
            return true;
        case ANVIL_OP_UITOFP:
            *out_op = ANVIL_MIR_OP_UITOFP;
            return true;
        case ANVIL_OP_FPTOSI:
            *out_op = ANVIL_MIR_OP_FPTOSI;
            return true;
        case ANVIL_OP_FPTOUI:
            *out_op = ANVIL_MIR_OP_FPTOUI;
            return true;
        case ANVIL_OP_FPEXT:
            *out_op = ANVIL_MIR_OP_FPEXT;
            return true;
        case ANVIL_OP_FPTRUNC:
            *out_op = ANVIL_MIR_OP_FPTRUNC;
            return true;
        default:
            return false;
    }
}

static const char *call_symbol(anvil_value_t *callee)
{
    if (!callee) return NULL;
    if (callee->kind == ANVIL_VAL_FUNC && callee->data.func) {
        return callee->data.func->name;
    }
    if (callee->kind == ANVIL_VAL_GLOBAL &&
        callee->type &&
        callee->type->kind == ANVIL_TYPE_FUNC &&
        callee->name) {
        return callee->name;
    }
    return NULL;
}

static bool call_is_direct_symbol(anvil_value_t *callee)
{
    if (!callee) return false;
    if (callee->kind == ANVIL_VAL_FUNC) return true;
    return callee->kind == ANVIL_VAL_GLOBAL &&
           callee->type &&
           callee->type->kind == ANVIL_TYPE_FUNC;
}

static anvil_type_t *call_func_type(anvil_value_t *callee)
{
    if (!callee || !callee->type) return NULL;
    if (callee->type->kind == ANVIL_TYPE_FUNC) return callee->type;
    if (callee->type->kind == ANVIL_TYPE_PTR &&
        callee->type->data.pointee &&
        callee->type->data.pointee->kind == ANVIL_TYPE_FUNC) {
        return callee->type->data.pointee;
    }
    return NULL;
}

static bool add_return_copy(arm64_mir_lower_t *lower, anvil_value_t *value)
{
    if (!value) {
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET,
                                   ANVIL_MIR_NO_VREG, NULL, 0);
    }

    anvil_mir_vreg_t src = lower_value(lower, value);
    if (src == ANVIL_MIR_NO_VREG) return false;

    anvil_mir_vreg_t ret = arm64_add_vreg_for_type(lower, value->type);
    if (ret == ANVIL_MIR_NO_VREG) return false;
    if (!anvil_mir_set_fixed_reg(lower->mir, ret, 0)) return false;

    anvil_mir_vreg_t uses[] = { src };
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, ret, uses, 1)) {
        return false;
    }

    anvil_mir_vreg_t ret_uses[] = { ret };
    return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET,
                               ANVIL_MIR_NO_VREG, ret_uses, 1);
}

static bool block_starts_with_phi(anvil_block_t *block)
{
    return block && block->first && block->first->op == ANVIL_OP_PHI;
}

static bool append_phi_edge_copy(phi_edge_copy_t **copies,
                                 size_t *num_copies,
                                 size_t *cap_copies,
                                 anvil_mir_vreg_t dst,
                                 anvil_mir_vreg_t src)
{
    if (dst == ANVIL_MIR_NO_VREG || src == ANVIL_MIR_NO_VREG) return false;
    if (dst == src) return true;

    if (*num_copies >= *cap_copies) {
        size_t new_cap = *cap_copies ? *cap_copies * 2 : 4;
        if (new_cap < *num_copies + 1) return false;

        phi_edge_copy_t *grown = realloc(*copies, new_cap * sizeof(*grown));
        if (!grown) return false;

        *copies = grown;
        *cap_copies = new_cap;
    }

    (*copies)[*num_copies].dst = dst;
    (*copies)[*num_copies].src = src;
    (*num_copies)++;
    return true;
}


static bool lower_phi_copies_for_edge(arm64_mir_lower_t *lower,
                                      anvil_block_t *src_block,
                                      anvil_block_t *dest_block)
{
    if (!block_starts_with_phi(dest_block)) return true;

    phi_edge_copy_t *copies = NULL;
    size_t num_copies = 0;
    size_t cap_copies = 0;

    for (anvil_instr_t *phi = dest_block->first; phi; phi = phi->next) {
        if (phi->op != ANVIL_OP_PHI) break;
        if (!phi->result) goto fail;

        anvil_mir_vreg_t phi_vreg = map_get(lower, phi->result);
        if (phi_vreg == ANVIL_MIR_NO_VREG) goto fail;

        bool found = false;
        for (size_t i = 0; i < phi->num_phi_incoming; i++) {
            if (phi->phi_blocks && phi->phi_blocks[i] == src_block) {
                if (i >= phi->num_operands || !phi->operands[i]) goto fail;

                anvil_mir_vreg_t incoming = lower_value(lower, phi->operands[i]);
                if (incoming == ANVIL_MIR_NO_VREG) goto fail;
                if (!append_phi_edge_copy(&copies, &num_copies, &cap_copies,
                                          phi_vreg, incoming)) {
                    goto fail;
                }
                found = true;
                break;
            }
        }

        if (!found) {
            goto fail;
        }
    }

    bool ok = anvil_mir_emit_parallel_copies(lower->mir, copies, num_copies);
    free(copies);
    return ok;

fail:
    free(copies);
    return false;
}

static anvil_mir_block_t create_phi_edge_block(arm64_mir_lower_t *lower,
                                               anvil_block_t *src_block,
                                               anvil_block_t *dest_block)
{
    const char *src_name = (src_block && src_block->name) ? src_block->name : "anon";
    const char *dest_name = (dest_block && dest_block->name) ? dest_block->name : "anon";

    char name[256];
    snprintf(name, sizeof(name), "%s_to_%s_phi_%zu",
             src_name, dest_name, lower->num_edge_blocks++);
    anvil_mir_block_t source = anvil_mir_current_block(lower->mir);
    anvil_mir_block_t edge = anvil_mir_add_block(lower->mir, name);
    if (!anvil_mir_set_current_block(lower->mir, source))
        return ANVIL_MIR_NO_BLOCK;

    return edge;
}

static bool emit_phi_edge_block(arm64_mir_lower_t *lower,
                                anvil_block_t *src_block,
                                anvil_block_t *dest_block,
                                anvil_mir_block_t edge_block,
                                anvil_mir_block_t dest_mir_block)
{
    if (!anvil_mir_set_current_block(lower->mir, edge_block)) return false;
    if (!lower_phi_copies_for_edge(lower, src_block, dest_block)) return false;
    return anvil_mir_add_branch(lower->mir, dest_mir_block);
}

static bool append_pending_phi_edge(pending_phi_edge_t **edges,
                                    size_t *num_edges,
                                    size_t *cap_edges,
                                    anvil_block_t *dest_block,
                                    anvil_mir_block_t edge_block,
                                    anvil_mir_block_t dest_mir_block)
{
    if (!dest_block || edge_block == ANVIL_MIR_NO_BLOCK ||
        dest_mir_block == ANVIL_MIR_NO_BLOCK) {
        return false;
    }

    if (*num_edges >= *cap_edges) {
        size_t new_cap = *cap_edges ? *cap_edges * 2 : 4;
        if (new_cap < *num_edges + 1) return false;

        pending_phi_edge_t *grown = realloc(*edges, new_cap * sizeof(*grown));
        if (!grown) return false;

        *edges = grown;
        *cap_edges = new_cap;
    }

    (*edges)[*num_edges].dest_block = dest_block;
    (*edges)[*num_edges].edge_block = edge_block;
    (*edges)[*num_edges].dest_mir_block = dest_mir_block;
    (*num_edges)++;
    return true;
}

static bool prepare_phi_aware_target(arm64_mir_lower_t *lower,
                                     anvil_block_t *src_block,
                                     anvil_block_t *dest_block,
                                     anvil_mir_block_t *out_target,
                                     pending_phi_edge_t **edges,
                                     size_t *num_edges,
                                     size_t *cap_edges)
{
    anvil_mir_block_t dest_mir = block_get(lower, dest_block);
    if (dest_mir == ANVIL_MIR_NO_BLOCK) return false;

    *out_target = dest_mir;
    if (!block_starts_with_phi(dest_block)) return true;

    anvil_mir_block_t edge_block =
        create_phi_edge_block(lower, src_block, dest_block);
    if (edge_block == ANVIL_MIR_NO_BLOCK) return false;
    *out_target = edge_block;
    return append_pending_phi_edge(edges, num_edges, cap_edges,
                                   dest_block, edge_block, dest_mir);
}

static bool emit_pending_phi_edges(arm64_mir_lower_t *lower,
                                   anvil_block_t *src_block,
                                   pending_phi_edge_t *edges,
                                   size_t num_edges)
{
    for (size_t i = 0; i < num_edges; i++) {
        if (!emit_phi_edge_block(lower, src_block, edges[i].dest_block,
                                 edges[i].edge_block,
                                 edges[i].dest_mir_block)) {
            return false;
        }
    }
    return true;
}

static anvil_mir_block_t create_switch_chain_block(arm64_mir_lower_t *lower,
                                                   anvil_block_t *src_block,
                                                   size_t case_index)
{
    const char *src_name = (src_block && src_block->name) ? src_block->name : "anon";

    char name[256];
    snprintf(name, sizeof(name), "%s_switch_case_%zu_%zu",
             src_name, case_index, lower->num_edge_blocks++);
    return anvil_mir_add_block(lower->mir, name);
}

static bool lower_call(arm64_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands == 0) return false;
    if (instr->call_cc != ANVIL_CC_SYSV) return false;

    anvil_value_t *callee = instr->operands[0];
    anvil_type_t *fn_type = call_func_type(callee);
    if (!fn_type) return false;

    bool direct_call = call_is_direct_symbol(callee);
    const char *symbol = direct_call ? call_symbol(callee) : NULL;
    if (direct_call && !symbol) return false;

    bool is_variadic = false;
    size_t num_fixed_args = 0;
    if (fn_type->data.func.variadic) {
        is_variadic = true;
        num_fixed_args = fn_type->data.func.num_params;
    }
    bool darwin_variadic = is_variadic && lower_abi(lower) == ANVIL_ABI_DARWIN;

    size_t num_args = instr->num_operands - 1;
    size_t max_call_uses = num_args + (direct_call ? 0 : 1);
    anvil_mir_vreg_t *call_uses = NULL;
    if (max_call_uses > 0) {
        call_uses = calloc(max_call_uses, sizeof(*call_uses));
        if (!call_uses) return false;
    }

    size_t gpr_count = 0;
    size_t fpr_count = 0;
    size_t num_call_uses = 0;
    int64_t stack_offset = 0;
    bool ok = true;

    if (!direct_call) {
        anvil_mir_vreg_t target_src = lower_value(lower, callee);
        anvil_mir_vreg_t target_fixed = arm64_add_vreg_for_type(lower, callee->type);
        if (target_src == ANVIL_MIR_NO_VREG ||
            target_fixed == ANVIL_MIR_NO_VREG ||
            !anvil_mir_set_fixed_reg(lower->mir, target_fixed, 16)) {
            free(call_uses);
            return false;
        }

        anvil_mir_vreg_t target_uses[] = { target_src };
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                                 target_fixed, target_uses, 1)) {
            free(call_uses);
            return false;
        }
        call_uses[num_call_uses++] = target_fixed;
    }

    for (size_t i = 0; i < num_args; i++) {
        anvil_value_t *arg = instr->operands[i + 1];
        anvil_mir_vreg_t src = lower_value(lower, arg);
        if (src == ANVIL_MIR_NO_VREG) {
            ok = false;
            break;
        }

        bool force_stack = darwin_variadic && i >= num_fixed_args;
        if (force_stack ||
            !arg_still_uses_register(arg->type, gpr_count, fpr_count)) {
            anvil_mir_vreg_t stack_use[] = { src };
            if (!anvil_mir_add_instr_imm_uses(lower->mir,
                                              ANVIL_MIR_OP_CALL_STACK_ARG,
                                              ANVIL_MIR_NO_VREG,
                                              stack_use, 1,
                                              stack_offset)) {
                ok = false;
                break;
            }
            stack_offset += 8;
            if (!force_stack) {
                advance_arg_count(arg->type, &gpr_count, &fpr_count);
            }
            continue;
        }

        anvil_mir_vreg_t fixed_arg = arm64_add_vreg_for_type(lower, arg->type);
        if (fixed_arg == ANVIL_MIR_NO_VREG ||
            !set_fixed_register_arg(lower, fixed_arg, arg->type,
                                    &gpr_count, &fpr_count)) {
            ok = false;
            break;
        }

        anvil_mir_vreg_t copy_uses[] = { src };
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                                 fixed_arg, copy_uses, 1)) {
            ok = false;
            break;
        }
        call_uses[num_call_uses++] = fixed_arg;
    }

    if (!ok) {
        free(call_uses);
        return false;
    }

    anvil_mir_vreg_t call_def = ANVIL_MIR_NO_VREG;
    if (instr->result && !arm64_type_is_void(instr->result->type)) {
        call_def = arm64_add_vreg_for_type(lower, instr->result->type);
        if (call_def == ANVIL_MIR_NO_VREG ||
            !anvil_mir_set_fixed_reg(lower->mir, call_def, 0)) {
            free(call_uses);
            return false;
        }
    }

    ok = anvil_mir_add_call(lower->mir, call_def, call_uses, num_call_uses,
                            symbol, instr->call_cc, false, 0);
    free(call_uses);
    if (!ok) return false;

    if (instr->result && call_def != ANVIL_MIR_NO_VREG) {
        anvil_mir_vreg_t local_result =
            arm64_add_vreg_for_type(lower, instr->result->type);
        if (local_result == ANVIL_MIR_NO_VREG) return false;

        anvil_mir_vreg_t copy_uses[] = { call_def };
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                                 local_result, copy_uses, 1)) {
            return false;
        }
        return map_put(lower, instr->result, local_result);
    }
    return true;
}

static anvil_mir_vreg_t lower_widen_gpr_to_64(arm64_mir_lower_t *lower,
                                              anvil_mir_vreg_t src,
                                              bool sign_extend)
{
    const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->mir, src);
    if (!src_info || src_info->reg_class != ANVIL_MIR_REG_GPR) {
        return ANVIL_MIR_NO_VREG;
    }
    if (src_info->size_bits >= 64) return src;

    anvil_mir_vreg_t wide =
        anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 64,
                                 sign_extend);
    if (wide == ANVIL_MIR_NO_VREG) return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t uses[] = { src };
    anvil_mir_opcode_t op = sign_extend ? ANVIL_MIR_OP_SEXT
                                        : ANVIL_MIR_OP_ZEXT;
    if (!anvil_mir_add_instr(lower->mir, op, wide, uses, 1)) {
        return ANVIL_MIR_NO_VREG;
    }
    return wide;
}

static anvil_mir_vreg_t lower_resize_gpr(arm64_mir_lower_t *lower,
                                         anvil_mir_vreg_t src,
                                         uint16_t target_bits)
{
    const anvil_mir_vreg_info_t *src_info =
        anvil_mir_get_vreg_info(lower->mir, src);
    if (!src_info || src_info->reg_class != ANVIL_MIR_REG_GPR ||
        src_info->size_bits == 0 || target_bits == 0) {
        return ANVIL_MIR_NO_VREG;
    }
    if (src_info->size_bits == target_bits) return src;

    /* Capture src fields before anvil_mir_add_vreg_typed: it may realloc the
     * vreg array, invalidating src_info. */
    uint16_t src_bits = src_info->size_bits;
    bool src_signed = src_info->is_signed;

    anvil_mir_vreg_t resized =
        anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR,
                                 target_bits, src_signed);
    if (resized == ANVIL_MIR_NO_VREG) return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t uses[] = { src };
    anvil_mir_opcode_t op = ANVIL_MIR_OP_TRUNC;
    if (src_bits < target_bits) {
        op = src_signed ? ANVIL_MIR_OP_SEXT : ANVIL_MIR_OP_ZEXT;
    }
    if (!anvil_mir_add_instr(lower->mir, op, resized, uses, 1)) {
        return ANVIL_MIR_NO_VREG;
    }
    return resized;
}

static bool lower_match_binary_operand_sizes(arm64_mir_lower_t *lower,
                                             anvil_mir_vreg_t *lhs,
                                             anvil_mir_vreg_t *rhs,
                                             anvil_mir_vreg_t def)
{
    const anvil_mir_vreg_info_t *def_info =
        anvil_mir_get_vreg_info(lower->mir, def);
    const anvil_mir_vreg_info_t *lhs_info =
        anvil_mir_get_vreg_info(lower->mir, *lhs);
    const anvil_mir_vreg_info_t *rhs_info =
        anvil_mir_get_vreg_info(lower->mir, *rhs);
    if (!def_info || !lhs_info || !rhs_info) return false;

    if (def_info->reg_class != ANVIL_MIR_REG_GPR ||
        lhs_info->reg_class != ANVIL_MIR_REG_GPR ||
        rhs_info->reg_class != ANVIL_MIR_REG_GPR) {
        return true;
    }

    /* Capture def size before resizing: lower_resize_gpr may realloc the vreg
     * array, invalidating def_info. */
    uint16_t def_bits = def_info->size_bits;
    *lhs = lower_resize_gpr(lower, *lhs, def_bits);
    *rhs = lower_resize_gpr(lower, *rhs, def_bits);
    return *lhs != ANVIL_MIR_NO_VREG && *rhs != ANVIL_MIR_NO_VREG;
}

static bool lower_alloca(arm64_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || !instr->result->type ||
        instr->result->type->kind != ANVIL_TYPE_PTR) {
        return false;
    }

    anvil_type_t *element_type = instr->aux_type;
    if (!element_type) {
        element_type = instr->result->type->data.pointee;
    }
    if (!element_type) return false;

    anvil_mir_vreg_t ptr = arm64_add_vreg_for_type(lower, instr->result->type);
    if (ptr == ANVIL_MIR_NO_VREG) return false;

    if (instr->num_operands == 0) {
        int slot = anvil_mir_add_frame_slot(lower->mir,
                                            arm64_slot_bits_for_type(element_type),
                                            arm64_align_for_type(element_type));
        if (slot < 0) return false;
        if (!anvil_mir_add_frame_addr(lower->mir, ptr, slot)) return false;
        return map_put(lower, instr->result, ptr);
    }

    if (instr->num_operands != 1) return false;
    anvil_mir_vreg_t count = lower_value(lower, instr->operands[0]);
    if (count == ANVIL_MIR_NO_VREG) return false;
    count = lower_widen_gpr_to_64(lower, count, false);
    if (count == ANVIL_MIR_NO_VREG) return false;

    int64_t elem_size = arm64_type_size(element_type);
    if (elem_size <= 0) elem_size = 1;
    anvil_mir_vreg_t uses[] = { count };
    if (!anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_DYN_ALLOCA,
                                      ptr, uses, 1, elem_size)) {
        return false;
    }
    return map_put(lower, instr->result, ptr);
}

static bool lower_add_const_offset(arm64_mir_lower_t *lower,
                                   anvil_mir_vreg_t base,
                                   int64_t offset,
                                   anvil_mir_vreg_t *out_ptr)
{
    if (offset == 0) {
        *out_ptr = base;
        return true;
    }

    anvil_mir_vreg_t off =
        anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t dst =
        anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 64);
    if (off == ANVIL_MIR_NO_VREG || dst == ANVIL_MIR_NO_VREG) return false;
    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, off, offset)) {
        return false;
    }
    anvil_mir_vreg_t uses[] = { base, off };
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_ADD, dst, uses, 2)) {
        return false;
    }

    *out_ptr = dst;
    return true;
}

static bool lower_gep(arm64_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands < 2 || !instr->result || !instr->aux_type)
        return false;

    anvil_mir_vreg_t current = lower_value(lower, instr->operands[0]);
    if (current == ANVIL_MIR_NO_VREG) return false;
    current = lower_widen_gpr_to_64(lower, current, false);
    if (current == ANVIL_MIR_NO_VREG) return false;

    anvil_type_t *walk_type = instr->aux_type;
    bool all_constant = true;
    int64_t constant_offset = 0;
    for (size_t i = 1; i < instr->num_operands; i++) {
        anvil_value_t *index_value = instr->operands[i];
        anvil_gep_step_t step;
        if (!anvil_gep_analyze_step(&walk_type, index_value, i - 1, &step) ||
            step.amount > (size_t)INT64_MAX) return false;
        if (index_value->kind == ANVIL_VAL_CONST_INT) {
            int64_t offset;
            if (!anvil_gep_const_step_offset(&step, index_value, &offset) ||
                !anvil_gep_accumulate_offset(&constant_offset, offset)) {
                return false;
            }
            continue;
        }
        all_constant = false;
        if (step.kind != ANVIL_GEP_STEP_SCALE) return false;
        int64_t elem_size = (int64_t)step.amount;
        anvil_mir_vreg_t index = lower_value(lower, index_value);
        if (index == ANVIL_MIR_NO_VREG) return false;
        index = lower_widen_gpr_to_64(lower, index,
                                      index_value->type->is_signed);
        if (index == ANVIL_MIR_NO_VREG) return false;

        anvil_mir_vreg_t scaled = index;
        if (elem_size != 1) {
            anvil_mir_vreg_t scale =
                anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 64);
            scaled = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 64);
            if (scale == ANVIL_MIR_NO_VREG ||
                scaled == ANVIL_MIR_NO_VREG) {
                return false;
            }
            if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV,
                                         scale, elem_size)) {
                return false;
            }
            anvil_mir_vreg_t mul_uses[] = { index, scale };
            if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_MUL,
                                     scaled, mul_uses, 2)) {
                return false;
            }
        }

        anvil_mir_vreg_t next =
            anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 64);
        if (next == ANVIL_MIR_NO_VREG) return false;
        anvil_mir_vreg_t add_uses[] = { current, scaled };
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_ADD,
                                 next, add_uses, 2)) {
            return false;
        }
        current = next;
    }
    if (all_constant)
        return addr_map_put(lower, instr->result, current, constant_offset);
    if (!lower_add_const_offset(lower, current, constant_offset, &current))
        return false;
    return map_put(lower, instr->result, current);
}

static bool lower_struct_gep(arm64_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands < 1 || !instr->result) return false;

    anvil_mir_vreg_t base = lower_value(lower, instr->operands[0]);
    if (base == ANVIL_MIR_NO_VREG) return false;
    base = lower_widen_gpr_to_64(lower, base, false);
    if (base == ANVIL_MIR_NO_VREG) return false;

    int64_t offset = 0;
    if (instr->aux_type && instr->aux_type->kind == ANVIL_TYPE_STRUCT &&
        instr->num_operands > 1 &&
        instr->operands[1] &&
        instr->operands[1]->kind == ANVIL_VAL_CONST_INT) {
        unsigned idx = (unsigned)instr->operands[1]->data.i;
        if (idx >= instr->aux_type->data.struc.num_fields) return false;
        offset = (int64_t)instr->aux_type->data.struc.offsets[idx];
    }

    return addr_map_put(lower, instr->result, base, offset);
}

static bool lower_cast(arm64_mir_lower_t *lower,
                       anvil_instr_t *instr,
                       anvil_mir_opcode_t mir_op)
{
    if (instr->num_operands != 1 || !instr->result) return false;
    anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t def = arm64_add_vreg_for_type(lower, instr->result->type);
    if (src == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) return false;

    if (instr->result->type->kind == ANVIL_TYPE_I1 &&
        instr->operands[0]->type->kind != ANVIL_TYPE_I1) {
        const anvil_mir_vreg_info_t *src_info =
            anvil_mir_get_vreg_info(lower->mir, src);
        if (src_info && src_info->reg_class == ANVIL_MIR_REG_FPR) {
            if (mir_op != ANVIL_MIR_OP_FPTOUI) return false;
            anvil_mir_vreg_t converted = anvil_mir_add_vreg_typed(
                lower->mir, ANVIL_MIR_REG_GPR, 32, false);
            anvil_mir_vreg_t convert_use[] = { src };
            if (converted == ANVIL_MIR_NO_VREG ||
                !anvil_mir_add_instr(lower->mir, mir_op, converted,
                                     convert_use, 1)) return false;
            src = converted;
            src_info = anvil_mir_get_vreg_info(lower->mir, src);
        }
        anvil_mir_vreg_t narrowed = src;
        if (!src_info || src_info->reg_class != ANVIL_MIR_REG_GPR) return false;
        if (src_info->size_bits > 8) {
            narrowed = anvil_mir_add_vreg_typed(lower->mir,
                ANVIL_MIR_REG_GPR, 8, false);
            anvil_mir_vreg_t narrow_use[] = { src };
            if (narrowed == ANVIL_MIR_NO_VREG ||
                !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_TRUNC,
                                     narrowed, narrow_use, 1)) return false;
        }
        anvil_mir_vreg_t one = anvil_mir_add_vreg_typed(
            lower->mir, ANVIL_MIR_REG_GPR, 8, false);
        anvil_mir_vreg_t uses[] = { narrowed, one };
        if (one == ANVIL_MIR_NO_VREG ||
            !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, one, 1) ||
            !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_AND, def, uses, 2))
            return false;
        return map_put(lower, instr->result, def);
    }

    if (mir_op == ANVIL_MIR_OP_BITCAST) {
        const anvil_mir_vreg_info_t *src_info =
            anvil_mir_get_vreg_info(lower->mir, src);
        const anvil_mir_vreg_info_t *dst_info =
            anvil_mir_get_vreg_info(lower->mir, def);
        if (!src_info || !dst_info) return false;
        if (src_info->size_bits != dst_info->size_bits) {
            if (instr->op != ANVIL_OP_PTRTOINT &&
                instr->op != ANVIL_OP_INTTOPTR) {
                return false;
            }
            mir_op = src_info->size_bits < dst_info->size_bits
                ? ANVIL_MIR_OP_ZEXT : ANVIL_MIR_OP_TRUNC;
        }
    }

    anvil_mir_vreg_t uses[] = { src };
    if (!anvil_mir_add_instr(lower->mir, mir_op, def, uses, 1)) return false;
    return map_put(lower, instr->result, def);
}

static bool lower_select(arm64_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands != 3 || !instr->result) return false;
    anvil_mir_vreg_t cond = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t then_v = lower_value(lower, instr->operands[1]);
    anvil_mir_vreg_t else_v = lower_value(lower, instr->operands[2]);
    anvil_mir_vreg_t def = arm64_add_vreg_for_type(lower, instr->result->type);
    if (cond == ANVIL_MIR_NO_VREG ||
        then_v == ANVIL_MIR_NO_VREG ||
        else_v == ANVIL_MIR_NO_VREG ||
        def == ANVIL_MIR_NO_VREG) {
        return false;
    }

    anvil_mir_vreg_t uses[] = { cond, then_v, else_v };
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_SELECT,
                             def, uses, 3)) {
        return false;
    }
    return map_put(lower, instr->result, def);
}

static bool lower_memory_address(arm64_mir_lower_t *lower,
                                 anvil_value_t *value,
                                 anvil_mir_vreg_t *out_base,
                                 int64_t *out_offset)
{
    anvil_mir_vreg_t base = ANVIL_MIR_NO_VREG;
    int64_t offset = 0;
    if (addr_map_get(lower, value, &base, &offset)) {
        base = lower_widen_gpr_to_64(lower, base, false);
        if (base == ANVIL_MIR_NO_VREG) return false;
        *out_base = base;
        *out_offset = offset;
        return true;
    }

    base = lower_value(lower, value);
    if (base == ANVIL_MIR_NO_VREG) return false;
    base = lower_widen_gpr_to_64(lower, base, false);
    if (base == ANVIL_MIR_NO_VREG) return false;

    *out_base = base;
    *out_offset = 0;
    return true;
}

static bool lower_switch(arm64_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands != instr->num_switch_cases + 1 ||
        instr->num_operands < 1 ||
        !instr->true_block) {
        return false;
    }

    anvil_mir_vreg_t selector = lower_value(lower, instr->operands[0]);
    if (selector == ANVIL_MIR_NO_VREG) return false;

    pending_phi_edge_t *edges = NULL;
    size_t num_edges = 0;
    size_t cap_edges = 0;

    anvil_mir_block_t default_target = ANVIL_MIR_NO_BLOCK;
    if (!prepare_phi_aware_target(lower, instr->parent, instr->true_block,
                                  &default_target, &edges, &num_edges,
                                  &cap_edges)) {
        free(edges);
        return false;
    }

    if (instr->num_switch_cases == 0) {
        bool ok = anvil_mir_add_branch(lower->mir, default_target) &&
                  emit_pending_phi_edges(lower, instr->parent, edges,
                                         num_edges);
        free(edges);
        return ok;
    }

    anvil_mir_block_t current_block = anvil_mir_current_block(lower->mir);
    bool ok = true;
    for (size_t i = 0; i < instr->num_switch_cases; i++) {
        if (!anvil_mir_set_current_block(lower->mir, current_block)) {
            ok = false;
            break;
        }

        anvil_mir_block_t case_target = ANVIL_MIR_NO_BLOCK;
        if (!prepare_phi_aware_target(lower, instr->parent,
                                      instr->switch_blocks[i],
                                      &case_target, &edges, &num_edges,
                                      &cap_edges)) {
            ok = false;
            break;
        }

        anvil_mir_block_t false_target = default_target;
        anvil_mir_block_t next_chain = ANVIL_MIR_NO_BLOCK;
        if (i + 1 < instr->num_switch_cases) {
            next_chain = create_switch_chain_block(lower, instr->parent, i + 1);
            if (next_chain == ANVIL_MIR_NO_BLOCK) {
                ok = false;
                break;
            }
            false_target = next_chain;
            if (!anvil_mir_set_current_block(lower->mir, current_block)) {
                ok = false;
                break;
            }
        }

        anvil_mir_vreg_t case_value = lower_value(lower, instr->operands[i + 1]);
        anvil_mir_vreg_t cmp =
            anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
        if (case_value == ANVIL_MIR_NO_VREG || cmp == ANVIL_MIR_NO_VREG) {
            ok = false;
            break;
        }

        anvil_mir_vreg_t cmp_uses[] = { selector, case_value };
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_CMP_EQ,
                                 cmp, cmp_uses, 2) ||
            !anvil_mir_add_cond_branch(lower->mir, cmp,
                                       case_target, false_target)) {
            ok = false;
            break;
        }

        current_block = next_chain;
    }

    if (ok) {
        ok = emit_pending_phi_edges(lower, instr->parent, edges, num_edges);
    }
    free(edges);
    return ok;
}

static bool lower_instr(arm64_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->op == ANVIL_OP_NOP) return true;
    if (instr->op == ANVIL_OP_PHI) return true;

    if (anvil_op_is_atomic(instr->op))
    {
        anvil_mir_vreg_t uses[3];
        for (size_t operand = 0; operand < instr->num_operands; operand++)
        {
            uses[operand] = lower_value(lower, instr->operands[operand]);
            if (uses[operand] == ANVIL_MIR_NO_VREG)
                return false;
        }

        anvil_mir_vreg_t def = instr->result ? arm64_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
        if ((instr->result && def == ANVIL_MIR_NO_VREG) ||
            !anvil_mir_add_atomic(lower->mir, instr->op, def, uses, instr->num_operands, &instr->atomic))
            return false;

        return !instr->result || map_put(lower, instr->result, def);
    }

    anvil_mir_opcode_t mir_op;
    if (instr->num_operands == 2 && map_binop(instr->op, &mir_op)) {
        anvil_mir_vreg_t lhs = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t rhs = lower_value(lower, instr->operands[1]);
        anvil_mir_vreg_t def = instr->result
            ? arm64_add_vreg_for_type(lower, instr->result->type)
            : ANVIL_MIR_NO_VREG;
        if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG ||
            def == ANVIL_MIR_NO_VREG) {
            return false;
        }
        if (!mir_op_is_compare(mir_op) &&
            !lower_match_binary_operand_sizes(lower, &lhs, &rhs, def)) {
            return false;
        }

        anvil_mir_vreg_t uses[] = { lhs, rhs };
        bool added = mir_op == ANVIL_MIR_OP_FCMP
            ? anvil_mir_add_instr_imm_uses(lower->mir, mir_op, def, uses, 2,
                                           instr->fcmp_pred)
            : anvil_mir_add_instr(lower->mir, mir_op, def, uses, 2);
        if (!added) {
            return false;
        }
        return map_put(lower, instr->result, def);
    }

    if (instr->num_operands == 1 && map_unop(instr->op, &mir_op)) {
        anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t def = instr->result
            ? arm64_add_vreg_for_type(lower, instr->result->type)
            : ANVIL_MIR_NO_VREG;
        if (src == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) return false;

        anvil_mir_vreg_t uses[] = { src };
        if (!anvil_mir_add_instr(lower->mir, mir_op, def, uses, 1)) return false;
        return map_put(lower, instr->result, def);
    }

    if (instr->num_operands == 1 && map_cast(instr->op, &mir_op)) {
        return lower_cast(lower, instr, mir_op);
    }

    switch (instr->op) {
        case ANVIL_OP_VA_START: {
            if (!instr->result || !lower->func->type->data.func.variadic || lower_abi(lower) != ANVIL_ABI_SYSV ||
                lower->func->num_params > INT32_MAX / 8)
                return false;

            size_t gpr = 0;
            size_t fpr = 0;
            size_t stack = 0;
            for (size_t parameter = 0; parameter < lower->func->num_params; parameter++)
            {
                anvil_type_t *type = lower->func->params[parameter]->type;
                if (!arg_still_uses_register(type, gpr, fpr))
                    stack += (size_t)arm64_stack_arg_slot_size(type);

                advance_arg_count(type, &gpr, &fpr);
            }

            anvil_mir_vreg_t def = arm64_add_vreg_for_type(lower, instr->result->type);
            int slot = anvil_mir_add_frame_slot(lower->mir, 224 * 8, 16);
            return def != ANVIL_MIR_NO_VREG && slot >= 0 &&
                   anvil_mir_add_va_start(lower->mir, def, slot, (unsigned)(gpr < 8 ? gpr : 8), (unsigned)(fpr < 8 ? fpr : 8), stack) &&
                   map_put(lower, instr->result, def);
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
            if (instr->num_operands != 1 || !instr->result) return false;
            anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
            int64_t offset = 0;
            if (!lower_memory_address(lower, instr->operands[0],
                                      &ptr, &offset)) {
                return false;
            }
            anvil_mir_vreg_t def = arm64_add_vreg_for_type(lower,
                                                           instr->result->type);
            if (ptr == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) return false;
            bool is_i1 = instr->result->type->kind == ANVIL_TYPE_I1;
            anvil_mir_vreg_t loaded = is_i1
                ? anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false)
                : def;
            if (loaded == ANVIL_MIR_NO_VREG) return false;
            anvil_mir_vreg_t uses[] = { ptr };
            bool ok = offset == 0
                ? anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_LOAD, loaded,
                                      uses, 1)
                : anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_LOAD,
                                               loaded, uses, 1, offset);
            if (!ok) {
                return false;
            }
            if (is_i1) {
                anvil_mir_vreg_t one = anvil_mir_add_vreg_typed(
                    lower->mir, ANVIL_MIR_REG_GPR, 8, false);
                anvil_mir_vreg_t norm_uses[] = { loaded, one };
                if (one == ANVIL_MIR_NO_VREG ||
                    !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, one, 1) ||
                    !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_AND, def,
                                         norm_uses, 2)) return false;
            }
            return map_put(lower, instr->result, def);
        }
        case ANVIL_OP_STORE: {
            if (instr->num_operands != 2) return false;
            anvil_mir_vreg_t val = lower_value(lower, instr->operands[0]);
            anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
            int64_t offset = 0;
            if (!lower_memory_address(lower, instr->operands[1],
                                      &ptr, &offset)) {
                return false;
            }
            if (val == ANVIL_MIR_NO_VREG || ptr == ANVIL_MIR_NO_VREG) return false;
            if (instr->operands[0]->type->kind == ANVIL_TYPE_I1) {
                anvil_mir_vreg_t one = anvil_mir_add_vreg_typed(
                    lower->mir, ANVIL_MIR_REG_GPR, 8, false);
                anvil_mir_vreg_t normalized = anvil_mir_add_vreg_typed(
                    lower->mir, ANVIL_MIR_REG_GPR, 8, false);
                anvil_mir_vreg_t norm_uses[] = { val, one };
                if (one == ANVIL_MIR_NO_VREG || normalized == ANVIL_MIR_NO_VREG ||
                    !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, one, 1) ||
                    !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_AND, normalized,
                                         norm_uses, 2)) return false;
                val = normalized;
            }
            anvil_mir_vreg_t uses[] = { val, ptr };
            if (offset == 0) {
                return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_STORE,
                                           ANVIL_MIR_NO_VREG, uses, 2);
            }
            return anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_STORE,
                                                ANVIL_MIR_NO_VREG, uses, 2,
                                                offset);
        }
        case ANVIL_OP_CALL:
            return lower_call(lower, instr);
        case ANVIL_OP_BR: {
            if (!instr->true_block) return false;
            anvil_mir_block_t target = block_get(lower, instr->true_block);
            if (target == ANVIL_MIR_NO_BLOCK) return false;
            if (!lower_phi_copies_for_edge(lower, instr->parent, instr->true_block)) {
                return false;
            }
            return anvil_mir_add_branch(lower->mir, target);
        }
        case ANVIL_OP_BR_COND: {
            if (instr->num_operands != 1 || !instr->true_block || !instr->false_block) {
                return false;
            }
            anvil_mir_vreg_t cond = lower_value(lower, instr->operands[0]);
            anvil_mir_block_t true_block = block_get(lower, instr->true_block);
            anvil_mir_block_t false_block = block_get(lower, instr->false_block);
            if (cond == ANVIL_MIR_NO_VREG ||
                true_block == ANVIL_MIR_NO_BLOCK ||
                false_block == ANVIL_MIR_NO_BLOCK) {
                return false;
            }

            anvil_mir_block_t source_block = anvil_mir_current_block(lower->mir);
            anvil_mir_block_t true_target = true_block;
            anvil_mir_block_t false_target = false_block;
            anvil_mir_block_t true_edge = ANVIL_MIR_NO_BLOCK;
            anvil_mir_block_t false_edge = ANVIL_MIR_NO_BLOCK;

            if (block_starts_with_phi(instr->true_block)) {
                true_edge = create_phi_edge_block(lower, instr->parent,
                                                  instr->true_block);
                if (true_edge == ANVIL_MIR_NO_BLOCK) return false;
                true_target = true_edge;
            }

            if (block_starts_with_phi(instr->false_block)) {
                false_edge = create_phi_edge_block(lower, instr->parent,
                                                   instr->false_block);
                if (false_edge == ANVIL_MIR_NO_BLOCK) return false;
                false_target = false_edge;
            }

            if (!anvil_mir_set_current_block(lower->mir, source_block)) {
                return false;
            }

            if (!anvil_mir_add_cond_branch(lower->mir, cond,
                                           true_target, false_target)) {
                return false;
            }

            if (true_edge != ANVIL_MIR_NO_BLOCK &&
                !emit_phi_edge_block(lower, instr->parent, instr->true_block,
                                     true_edge, true_block)) {
                return false;
            }

            if (false_edge != ANVIL_MIR_NO_BLOCK &&
                !emit_phi_edge_block(lower, instr->parent, instr->false_block,
                                     false_edge, false_block)) {
                return false;
            }

            return anvil_mir_set_current_block(lower->mir, source_block);
        }
        case ANVIL_OP_SWITCH:
            return lower_switch(lower, instr);
        case ANVIL_OP_RET:
            if (instr->num_operands == 0) return add_return_copy(lower, NULL);
            if (instr->num_operands == 1) return add_return_copy(lower, instr->operands[0]);
            return false;
        default:
            return false;
    }
}

anvil_mir_func_t *anvil_arm64_lower_func_to_mir(anvil_func_t *func)
{
    if (!func || func->is_declaration || !func->type ||
        func->type->kind != ANVIL_TYPE_FUNC ||
        func->type->data.func.cc != ANVIL_CC_SYSV) return NULL;

    arm64_mir_lower_t lower;
    memset(&lower, 0, sizeof(lower));
    lower.func = func;
    lower.mir = anvil_mir_func_create(func->name);
    if (!lower.mir)
        return NULL;

    anvil_opt_cfg_t cfg;
    if (!anvil_opt_cfg_build(func, &cfg))
    {
        anvil_mir_func_destroy(lower.mir);
        return NULL;
    }

    if (!create_mir_blocks(&lower) ||
        !lower_params(&lower) ||
        !prepare_phi_results(&lower)) {
        anvil_opt_cfg_destroy(&cfg);
        anvil_mir_func_destroy(lower.mir);
        free(lower.blocks);
        free(lower.values);
        free(lower.addr_offsets);
        return NULL;
    }

    for (size_t rank = 0; rank < cfg.count; rank++)
    {
        anvil_block_t *block = cfg.blocks[cfg.rpo[rank]];
        anvil_mir_block_t mir_block = block_get(&lower, block);
        if (mir_block == ANVIL_MIR_NO_BLOCK ||
            !anvil_mir_set_current_block(lower.mir, mir_block)) {
            anvil_opt_cfg_destroy(&cfg);
            anvil_mir_func_destroy(lower.mir);
            free(lower.blocks);
            free(lower.values);
            free(lower.addr_offsets);
            return NULL;
        }

        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            size_t first = anvil_mir_num_instrs(lower.mir);
            if (!lower_instr(&lower, instr) || !anvil_mir_annotate_memory(lower.mir, first, &instr->memory_access) ||
                (instr->op == ANVIL_OP_CALL && !anvil_mir_annotate_call_effects(lower.mir, first, anvil_instr_call_effects(instr)))) {
                anvil_opt_cfg_destroy(&cfg);
                anvil_mir_func_destroy(lower.mir);
                free(lower.blocks);
                free(lower.values);
                free(lower.addr_offsets);
                return NULL;
            }
        }
    }

    free(lower.blocks);
    free(lower.values);
    free(lower.addr_offsets);
    anvil_opt_cfg_destroy(&cfg);
    return lower.mir;
}

static bool arm64_legal_fail(char *error, size_t error_len,
                             const char *fmt, ...)
{
    if (error && error_len > 0) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(error, error_len, fmt, args);
        va_end(args);
    }
    return false;
}

static bool arm64_legal_size_for_class(const anvil_mir_vreg_info_t *info)
{
    if (!info) return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR) {
        return info->size_bits == 8 ||
               info->size_bits == 16 ||
               info->size_bits == 32 ||
               info->size_bits == 64;
    }
    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        return info->size_bits == 32 || info->size_bits == 64;
    }
    return false;
}

static bool arm64_legal_fixed_reg(const anvil_mir_vreg_info_t *info)
{
    if (!info || !info->has_fixed_reg) return true;
    if (info->fixed_phys_reg < 0) return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR) return info->fixed_phys_reg <= 30;
    if (info->reg_class == ANVIL_MIR_REG_FPR) return info->fixed_phys_reg <= 31;
    return false;
}

static const anvil_mir_vreg_info_t *arm64_legal_vreg_info(
    const anvil_mir_func_t *mir,
    anvil_mir_vreg_t vreg,
    size_t instr_index,
    char *error,
    size_t error_len)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, vreg);
    if (!info) {
        arm64_legal_fail(error, error_len,
                         "ARM64 MIR instruction %zu uses invalid vreg",
                         instr_index);
        return NULL;
    }
    if (!arm64_legal_size_for_class(info)) {
        arm64_legal_fail(error, error_len,
                         "ARM64 MIR instruction %zu uses unsupported vreg class/size",
                         instr_index);
        return NULL;
    }
    if (!arm64_legal_fixed_reg(info)) {
        arm64_legal_fail(error, error_len,
                         "ARM64 MIR instruction %zu uses invalid fixed register",
                         instr_index);
        return NULL;
    }
    return info;
}

static bool arm64_legal_pointer_operand(const anvil_mir_func_t *mir,
                                        anvil_mir_vreg_t vreg,
                                        size_t instr_index,
                                        char *error,
                                        size_t error_len)
{
    const anvil_mir_vreg_info_t *info =
        arm64_legal_vreg_info(mir, vreg, instr_index, error, error_len);
    if (!info) return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR && info->size_bits == 64) {
        return true;
    }
    return arm64_legal_fail(error, error_len,
                            "ARM64 MIR instruction %zu requires a 64-bit pointer operand",
                            instr_index);
}

static bool arm64_legal_same_class_and_size(const anvil_mir_vreg_info_t *a,
                                            const anvil_mir_vreg_info_t *b)
{
    return a && b &&
           a->reg_class == b->reg_class &&
           a->size_bits == b->size_bits;
}

static bool arm64_legal_binary(const anvil_mir_func_t *mir,
                               size_t instr_index,
                               const anvil_mir_instr_info_t *instr,
                               char *error,
                               size_t error_len)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(mir, instr_index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(mir, instr_index, 1);
    const anvil_mir_vreg_info_t *def =
        arm64_legal_vreg_info(mir, instr->def, instr_index, error, error_len);
    const anvil_mir_vreg_info_t *lhs_info =
        arm64_legal_vreg_info(mir, lhs, instr_index, error, error_len);
    const anvil_mir_vreg_info_t *rhs_info =
        arm64_legal_vreg_info(mir, rhs, instr_index, error, error_len);
    if (!def || !lhs_info || !rhs_info) return false;

    if (!arm64_legal_same_class_and_size(def, lhs_info) ||
        !arm64_legal_same_class_and_size(lhs_info, rhs_info)) {
        return arm64_legal_fail(error, error_len,
                                "ARM64 MIR instruction %zu has incompatible binary operands",
                                instr_index);
    }

    bool fp_op = def->reg_class == ANVIL_MIR_REG_FPR;
    switch (instr->op) {
        case ANVIL_MIR_OP_ADD:
        case ANVIL_MIR_OP_SUB:
        case ANVIL_MIR_OP_MUL:
            return true;
        case ANVIL_MIR_OP_DIV:
        case ANVIL_MIR_OP_FDIV:
            return fp_op || def->reg_class == ANVIL_MIR_REG_GPR;
        case ANVIL_MIR_OP_SDIV:
        case ANVIL_MIR_OP_UDIV:
        case ANVIL_MIR_OP_SMOD:
        case ANVIL_MIR_OP_UMOD:
        case ANVIL_MIR_OP_AND:
        case ANVIL_MIR_OP_OR:
        case ANVIL_MIR_OP_XOR:
        case ANVIL_MIR_OP_SHL:
        case ANVIL_MIR_OP_SHR:
        case ANVIL_MIR_OP_SAR:
            if (def->reg_class == ANVIL_MIR_REG_GPR) return true;
            break;
        default:
            break;
    }

    return arm64_legal_fail(error, error_len,
                            "ARM64 MIR instruction %zu uses an illegal binary opcode/class pair",
                            instr_index);
}

static bool arm64_legal_call(const anvil_mir_func_t *mir,
                             size_t instr_index,
                             const anvil_mir_instr_info_t *instr,
                             char *error,
                             size_t error_len)
{
    if (instr->call_cc != ANVIL_CC_SYSV) {
        return arm64_legal_fail(error, error_len,
                                "ARM64 MIR call %zu uses an unsupported calling convention",
                                instr_index);
    }
    if (instr->def != ANVIL_MIR_NO_VREG) {
        const anvil_mir_vreg_info_t *def =
            arm64_legal_vreg_info(mir, instr->def, instr_index,
                                  error, error_len);
        if (!def) return false;
        if (!def->has_fixed_reg || def->fixed_phys_reg != 0) {
            return arm64_legal_fail(error, error_len,
                                    "ARM64 MIR call %zu result must be fixed to ABI result register",
                                    instr_index);
        }
    }

    size_t arg_start = 0;
    if (!instr->symbol || !instr->symbol[0]) {
        if (instr->num_uses == 0) {
            return arm64_legal_fail(error, error_len,
                                    "ARM64 MIR indirect call %zu requires a target register",
                                    instr_index);
        }

        anvil_mir_vreg_t target = anvil_mir_get_instr_use(mir, instr_index, 0);
        const anvil_mir_vreg_info_t *target_info =
            arm64_legal_vreg_info(mir, target, instr_index, error, error_len);
        if (!target_info) return false;
        if (target_info->reg_class != ANVIL_MIR_REG_GPR ||
            target_info->size_bits != 64 ||
            !target_info->has_fixed_reg ||
            target_info->fixed_phys_reg != 16) {
            return arm64_legal_fail(error, error_len,
                                    "ARM64 MIR indirect call %zu target must be fixed to x16",
                                    instr_index);
        }
        arg_start = 1;
    }

    for (size_t u = arg_start; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, instr_index, u);
        const anvil_mir_vreg_info_t *info =
            arm64_legal_vreg_info(mir, use, instr_index, error, error_len);
        if (!info) return false;
        if (!info->has_fixed_reg) {
            return arm64_legal_fail(error, error_len,
                                    "ARM64 MIR call %zu argument %zu must use a fixed ABI register",
                                    instr_index, u - arg_start);
        }
        if ((info->reg_class == ANVIL_MIR_REG_GPR ||
             info->reg_class == ANVIL_MIR_REG_FPR) &&
            info->fixed_phys_reg >= 0 && info->fixed_phys_reg < 8) {
            continue;
        }
        return arm64_legal_fail(error, error_len,
                                "ARM64 MIR call %zu argument %zu has an invalid fixed ABI register",
                                instr_index, u - arg_start);
    }

    return true;
}

static bool arm64_legal_instr(const anvil_mir_func_t *mir,
                              size_t instr_index,
                              const anvil_mir_instr_info_t *instr,
                              char *error,
                              size_t error_len)
{
    if (instr->def != ANVIL_MIR_NO_VREG &&
        !arm64_legal_vreg_info(mir, instr->def, instr_index,
                               error, error_len)) {
        return false;
    }
    for (size_t u = 0; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, instr_index, u);
        if (!arm64_legal_vreg_info(mir, use, instr_index,
                                   error, error_len)) {
            return false;
        }
    }

    switch (instr->op) {
        case ANVIL_MIR_OP_VA_START: {
            anvil_mir_frame_slot_info_t slot;
            return arm64_legal_pointer_operand(mir, instr->def, instr_index, error, error_len) &&
                   anvil_mir_get_frame_slot_info(mir, instr->frame_slot, &slot) && slot.size_bits >= 224 * 8 && slot.align_bytes >= 16 &&
                   instr->named_gpr <= 8 && instr->named_fpr <= 8 && instr->named_stack_bytes <= INT32_MAX - 16 && instr->named_stack_bytes % 8 == 0;
        }

        case ANVIL_MIR_OP_ATOMIC:
            for (size_t operand = 0; operand < instr->num_uses; operand++)
            {
                const anvil_mir_vreg_info_t *use = anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, instr_index, operand));
                if (use->has_fixed_reg && use->fixed_phys_reg >= 9 && use->fixed_phys_reg <= 11)
                    return arm64_legal_fail(error, error_len, "ARM64 atomic instruction %zu uses a reserved temporary", instr_index);
            }

            return instr->atomic_op == ANVIL_OP_ATOMIC_FENCE ||
                   arm64_legal_pointer_operand(mir, anvil_mir_get_instr_use(mir, instr_index, 0), instr_index, error, error_len);

        case ANVIL_MIR_OP_ADD:
        case ANVIL_MIR_OP_SUB:
        case ANVIL_MIR_OP_MUL:
        case ANVIL_MIR_OP_DIV:
        case ANVIL_MIR_OP_SDIV:
        case ANVIL_MIR_OP_UDIV:
        case ANVIL_MIR_OP_FDIV:
        case ANVIL_MIR_OP_SMOD:
        case ANVIL_MIR_OP_UMOD:
        case ANVIL_MIR_OP_AND:
        case ANVIL_MIR_OP_OR:
        case ANVIL_MIR_OP_XOR:
        case ANVIL_MIR_OP_SHL:
        case ANVIL_MIR_OP_SHR:
        case ANVIL_MIR_OP_SAR:
            return arm64_legal_binary(mir, instr_index, instr,
                                      error, error_len);

        case ANVIL_MIR_OP_LOAD: {
            anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(mir, instr_index, 0);
            return arm64_legal_pointer_operand(mir, ptr, instr_index,
                                               error, error_len);
        }

        case ANVIL_MIR_OP_STORE: {
            anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(mir, instr_index, 1);
            return arm64_legal_pointer_operand(mir, ptr, instr_index,
                                               error, error_len);
        }

        case ANVIL_MIR_OP_SYMBOL_ADDR:
        case ANVIL_MIR_OP_FRAME_ADDR:
        case ANVIL_MIR_OP_DYN_ALLOCA: {
            const anvil_mir_vreg_info_t *def =
                arm64_legal_vreg_info(mir, instr->def, instr_index,
                                      error, error_len);
            if (def && def->reg_class == ANVIL_MIR_REG_GPR &&
                def->size_bits == 64) {
                return true;
            }
            return arm64_legal_fail(error, error_len,
                                    "ARM64 MIR instruction %zu must define a 64-bit pointer",
                                    instr_index);
        }

        case ANVIL_MIR_OP_INCOMING_STACK_ARG:
        case ANVIL_MIR_OP_CALL_STACK_ARG:
            if (instr->has_imm && instr->imm >= 0 && (instr->imm % 8) == 0) {
                return true;
            }
            return arm64_legal_fail(error, error_len,
                                    "ARM64 MIR stack instruction %zu needs an aligned stack offset",
                                    instr_index);

        case ANVIL_MIR_OP_CALL:
            return arm64_legal_call(mir, instr_index, instr,
                                    error, error_len);

        case ANVIL_MIR_OP_SELECT: {
            const anvil_mir_vreg_info_t *def =
                arm64_legal_vreg_info(mir, instr->def, instr_index,
                                      error, error_len);
            const anvil_mir_vreg_info_t *then_info =
                arm64_legal_vreg_info(
                    mir, anvil_mir_get_instr_use(mir, instr_index, 1),
                    instr_index, error, error_len);
            const anvil_mir_vreg_info_t *else_info =
                arm64_legal_vreg_info(
                    mir, anvil_mir_get_instr_use(mir, instr_index, 2),
                    instr_index, error, error_len);
            if (arm64_legal_same_class_and_size(def, then_info) &&
                arm64_legal_same_class_and_size(then_info, else_info)) {
                return true;
            }
            return arm64_legal_fail(error, error_len,
                                    "ARM64 MIR select %zu has incompatible value operands",
                                    instr_index);
        }

        case ANVIL_MIR_OP_SPILL_LOAD:
        case ANVIL_MIR_OP_SPILL_STORE:
        case ANVIL_MIR_OP_MOV:
        case ANVIL_MIR_OP_COPY:
        case ANVIL_MIR_OP_NEG:
        case ANVIL_MIR_OP_NOT:
        case ANVIL_MIR_OP_FABS:
        case ANVIL_MIR_OP_ZEXT:
        case ANVIL_MIR_OP_SEXT:
        case ANVIL_MIR_OP_TRUNC:
        case ANVIL_MIR_OP_BITCAST:
        case ANVIL_MIR_OP_SITOFP:
        case ANVIL_MIR_OP_UITOFP:
        case ANVIL_MIR_OP_FPTOSI:
        case ANVIL_MIR_OP_FPTOUI:
        case ANVIL_MIR_OP_FPEXT:
        case ANVIL_MIR_OP_FPTRUNC:
        case ANVIL_MIR_OP_CMP:
        case ANVIL_MIR_OP_FCMP:
        case ANVIL_MIR_OP_CMP_EQ:
        case ANVIL_MIR_OP_CMP_NE:
        case ANVIL_MIR_OP_CMP_LT:
        case ANVIL_MIR_OP_CMP_LE:
        case ANVIL_MIR_OP_CMP_GT:
        case ANVIL_MIR_OP_CMP_GE:
        case ANVIL_MIR_OP_CMP_ULT:
        case ANVIL_MIR_OP_CMP_ULE:
        case ANVIL_MIR_OP_CMP_UGT:
        case ANVIL_MIR_OP_CMP_UGE:
        case ANVIL_MIR_OP_RET:
        case ANVIL_MIR_OP_BR:
        case ANVIL_MIR_OP_BR_COND:
        case ANVIL_MIR_OP_KEEPALIVE:
            return true;

        case ANVIL_MIR_OP_INVALID:
        default:
            break;
    }

    return arm64_legal_fail(error, error_len,
                            "ARM64 MIR instruction %zu uses unsupported opcode",
                            instr_index);
}

bool anvil_arm64_verify_mir_legal(const anvil_mir_func_t *mir,
                                  char *error,
                                  size_t error_len)
{
    if (error && error_len > 0) error[0] = '\0';
    if (!anvil_mir_verify(mir, error, error_len)) return false;

    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, i, &instr)) {
            return arm64_legal_fail(error, error_len,
                                    "ARM64 MIR instruction %zu is not inspectable",
                                    i);
        }
        if (!arm64_legal_instr(mir, i, &instr, error, error_len)) {
            return false;
        }
    }

    return true;
}

bool anvil_arm64_regalloc_mir(anvil_mir_func_t *mir)
{
    if (!mir) return false;

    for (size_t index = 0; index < anvil_mir_num_instrs(mir); index++)
    {
        anvil_mir_instr_info_t instruction;
        if (!anvil_mir_get_instr_info(mir, index, &instruction))
            return false;

        if (instruction.op == ANVIL_MIR_OP_ATOMIC)
        {
            uint64_t scratch = (UINT64_C(1) << 9) | (UINT64_C(1) << 10) | (UINT64_C(1) << 11);
            if (!anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_GPR, instruction.clobbers[ANVIL_MIR_REG_GPR] | scratch))
                return false;
        }

        if (instruction.op == ANVIL_MIR_OP_VA_START &&
            !anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_GPR, instruction.clobbers[ANVIL_MIR_REG_GPR] | (UINT64_C(1) << 16) | (UINT64_C(1) << 17)))
            return false;
    }

    static const int gpr_regs[] = {
        19, 20, 21, 22, 23, 24, 25, 26, 27, 28
    };
    static const int fpr_regs[] = {
        8, 9, 10, 11, 12, 13, 14, 15
    };
    static const int gpr_scratch_regs[] = {
        12, 13, 14, 15
    };
    static const int fpr_scratch_regs[] = {
        16, 17, 18, 19
    };

    anvil_regalloc_class_config_t configs[] = {
        { ANVIL_MIR_REG_GPR, (int)(sizeof(gpr_regs) / sizeof(gpr_regs[0])), gpr_regs },
        { ANVIL_MIR_REG_FPR, (int)(sizeof(fpr_regs) / sizeof(fpr_regs[0])), fpr_regs },
    };
    anvil_regalloc_class_config_t scratch_configs[] = {
        {
            ANVIL_MIR_REG_GPR,
            (int)(sizeof(gpr_scratch_regs) / sizeof(gpr_scratch_regs[0])),
            gpr_scratch_regs
        },
        {
            ANVIL_MIR_REG_FPR,
            (int)(sizeof(fpr_scratch_regs) / sizeof(fpr_scratch_regs[0])),
            fpr_scratch_regs
        },
    };

    if (!anvil_arm64_verify_mir_legal(mir, NULL, 0)) return false;
    if (!anvil_mir_coalesce_copies(mir)) return false;
    if (!anvil_arm64_verify_mir_legal(mir, NULL, 0)) return false;
    if (!anvil_regalloc_linear_scan_classes(
            mir, configs, sizeof(configs) / sizeof(configs[0]))) {
        return false;
    }
    if (!anvil_mir_materialize_spills(
            mir, scratch_configs,
            sizeof(scratch_configs) / sizeof(scratch_configs[0]))) {
        return false;
    }
    return anvil_arm64_verify_mir_legal(mir, NULL, 0);
}

typedef struct {
    const anvil_mir_func_t *mir;
    anvil_strbuf_t code;
    int *spill_offsets;
    size_t num_spill_offsets;
    int *frame_slot_offsets;
    size_t num_frame_slot_offsets;
    int gpr_save_offsets[32];
    int fpr_save_offsets[32];
    int outgoing_size;
    int frame_size;
    bool has_frame;
    bool is_darwin;
    bool failed;
} arm64_mir_emit_t;

static int align_int(int value, int align)
{
    return (value + align - 1) & ~(align - 1);
}

static int mir_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0) return 8;
    int size = (int)((size_bits + 7) / 8);
    if (size <= 0) return 8;
    if (size > 8) return 8;
    return size;
}

static int mir_slot_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0) return 8;
    int size = (int)((size_bits + 7) / 8);
    return size > 0 ? size : 8;
}

static const anvil_mir_vreg_info_t *mir_vreg_info_checked(
    arm64_mir_emit_t *emit,
    anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(emit->mir, vreg);
    if (!info) emit->failed = true;
    return info;
}

static const anvil_regalloc_assignment_t *mir_assignment_checked(
    arm64_mir_emit_t *emit,
    anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *assignment =
        anvil_mir_get_assignment(emit->mir, vreg);
    if (!assignment || assignment->spilled || assignment->phys_reg < 0) {
        emit->failed = true;
        return NULL;
    }
    return assignment;
}

static const char *mir_reg_name(arm64_mir_emit_t *emit,
                                anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = mir_vreg_info_checked(emit, vreg);
    const anvil_regalloc_assignment_t *assignment =
        mir_assignment_checked(emit, vreg);
    if (!info || !assignment) return "?";

    int size = mir_size_bytes(info->size_bits);
    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        if (assignment->phys_reg < 0 || assignment->phys_reg >= 32) {
            emit->failed = true;
            return "?";
        }
        return size <= 4 ? arm64_sreg_names[assignment->phys_reg]
                         : arm64_dreg_names[assignment->phys_reg];
    }

    if (assignment->phys_reg < 0 || assignment->phys_reg > 30) {
        emit->failed = true;
        return "?";
    }
    return size <= 4 ? arm64_wreg_names[assignment->phys_reg]
                     : arm64_xreg_names[assignment->phys_reg];
}

static const char *mir_gpr_scratch_name(uint16_t size_bits, int phys_reg)
{
    return mir_size_bytes(size_bits) <= 4 ? arm64_wreg_names[phys_reg]
                                          : arm64_xreg_names[phys_reg];
}

static const char *mir_gpr_name_for_size(int phys_reg, int size)
{
    return size <= 4 ? arm64_wreg_names[phys_reg] : arm64_xreg_names[phys_reg];
}

static void mir_emit_mov_gpr_imm(anvil_strbuf_t *code,
                                 const char *reg,
                                 int64_t imm)
{
    if (imm >= -65536 && imm <= 65535) {
        anvil_strbuf_appendf(code, "\tmov %s, #%lld\n", reg, (long long)imm);
    } else if ((uint64_t)imm <= UINT32_MAX) {
        uint32_t raw = (uint32_t)imm;
        uint32_t lo = raw & 0xffffu;
        uint32_t hi = (raw >> 16) & 0xffffu;
        anvil_strbuf_appendf(code, "\tmov %s, #%u\n", reg, lo);
        if (hi != 0) {
            anvil_strbuf_appendf(code, "\tmovk %s, #%u, lsl #16\n", reg, hi);
        }
    } else {
        anvil_strbuf_appendf(code, "\tldr %s, =%lld\n", reg, (long long)imm);
    }
}

static void mir_emit_stack_access(anvil_strbuf_t *code,
                                  const char *op,
                                  const char *reg,
                                  int offset)
{
    if (offset <= 255) {
        anvil_strbuf_appendf(code, "\t%s %s, [x29, #-%d]\n", op, reg, offset);
    } else if (offset <= 4095) {
        anvil_strbuf_appendf(code, "\tsub x16, x29, #%d\n", offset);
        anvil_strbuf_appendf(code, "\t%s %s, [x16]\n", op, reg);
    } else {
        anvil_strbuf_appendf(code, "\tldr x16, =%d\n", offset);
        anvil_strbuf_append(code, "\tsub x16, x29, x16\n");
        anvil_strbuf_appendf(code, "\t%s %s, [x16]\n", op, reg);
    }
}

static void mir_emit_sp_access(anvil_strbuf_t *code,
                               const char *op,
                               const char *reg,
                               int offset)
{
    if (offset < 0) return;

    if (offset <= 4095) {
        anvil_strbuf_appendf(code, "\t%s %s, [sp, #%d]\n", op, reg, offset);
    } else {
        anvil_strbuf_appendf(code, "\tldr x16, =%d\n", offset);
        anvil_strbuf_append(code, "\tadd x16, sp, x16\n");
        anvil_strbuf_appendf(code, "\t%s %s, [x16]\n", op, reg);
    }
}

static void mir_emit_incoming_stack_access(anvil_strbuf_t *code,
                                           const char *op,
                                           const char *reg,
                                           int offset)
{
    if (offset < 0) return;

    if (offset <= 4095) {
        anvil_strbuf_appendf(code, "\t%s %s, [x29, #%d]\n", op, reg, offset);
    } else {
        anvil_strbuf_appendf(code, "\tldr x16, =%d\n", offset);
        anvil_strbuf_append(code, "\tadd x16, x29, x16\n");
        anvil_strbuf_appendf(code, "\t%s %s, [x16]\n", op, reg);
    }
}

static const char *mir_load_op(anvil_mir_reg_class_t reg_class,
                               int size,
                               bool is_signed)
{
    if (reg_class == ANVIL_MIR_REG_FPR) return "ldr";
    if (is_signed) {
        switch (size) {
            case 1: return "ldrsb";
            case 2: return "ldrsh";
            case 4: return "ldrsw";
            default: break;
        }
    }
    switch (size) {
        case 1: return "ldrb";
        case 2: return "ldrh";
        default: return "ldr";
    }
}

static const char *mir_store_op(anvil_mir_reg_class_t reg_class, int size)
{
    if (reg_class == ANVIL_MIR_REG_FPR) return "str";
    switch (size) {
        case 1: return "strb";
        case 2: return "strh";
        default: return "str";
    }
}

static bool mir_offset_fits_scaled_address(int size, int64_t offset)
{
    if (offset < 0) return false;
    if (size <= 0) size = 1;
    return (offset % size) == 0 && (offset / size) <= 4095;
}

static void mir_emit_base_offset_access(anvil_strbuf_t *code,
                                        const char *op,
                                        const char *reg,
                                        const char *base,
                                        int size,
                                        int64_t offset)
{
    if (offset == 0) {
        anvil_strbuf_appendf(code, "\t%s %s, [%s]\n", op, reg, base);
    } else if (mir_offset_fits_scaled_address(size, offset)) {
        anvil_strbuf_appendf(code, "\t%s %s, [%s, #%lld]\n",
                             op, reg, base, (long long)offset);
    } else {
        anvil_strbuf_appendf(code, "\tldr x16, =%lld\n", (long long)offset);
        anvil_strbuf_appendf(code, "\tadd x16, %s, x16\n", base);
        anvil_strbuf_appendf(code, "\t%s %s, [x16]\n", op, reg);
    }
}

static bool mir_emit_label(arm64_mir_emit_t *emit, anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info)) return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s:\n",
                         anvil_mir_func_name(emit->mir), name);
    return true;
}

static bool mir_emit_branch_target(arm64_mir_emit_t *emit,
                                   anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info)) return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s",
                         anvil_mir_func_name(emit->mir), name);
    return true;
}

static const char *mir_symbol_prefix(const arm64_mir_emit_t *emit)
{
    return emit && emit->is_darwin ? "_" : "";
}

static bool mir_symbol_is_local(const char *symbol)
{
    return symbol && symbol[0] == '.';
}

static const char *mir_symbol_ref_prefix(const arm64_mir_emit_t *emit,
                                         const char *symbol)
{
    return mir_symbol_is_local(symbol) ? "" : mir_symbol_prefix(emit);
}

static const char *mir_cmp_cond(anvil_mir_opcode_t op)
{
    switch (op) {
        case ANVIL_MIR_OP_CMP_EQ:  return "eq";
        case ANVIL_MIR_OP_CMP_NE:
        case ANVIL_MIR_OP_CMP:     return "ne";
        case ANVIL_MIR_OP_CMP_LT:  return "lt";
        case ANVIL_MIR_OP_CMP_LE:  return "le";
        case ANVIL_MIR_OP_CMP_GT:  return "gt";
        case ANVIL_MIR_OP_CMP_GE:  return "ge";
        case ANVIL_MIR_OP_CMP_ULT: return "lo";
        case ANVIL_MIR_OP_CMP_ULE: return "ls";
        case ANVIL_MIR_OP_CMP_UGT: return "hi";
        case ANVIL_MIR_OP_CMP_UGE: return "hs";
        default: return "ne";
    }
}

static bool mir_instr_has_call(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info)) return true;
        if (info.op == ANVIL_MIR_OP_CALL) return true;
    }
    return false;
}

static bool mir_instr_has_dynamic_alloca(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info)) return true;
        if (info.op == ANVIL_MIR_OP_DYN_ALLOCA) return true;
    }
    return false;
}

static bool mir_instr_has_incoming_stack_arg(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info)) return true;
        if (info.op == ANVIL_MIR_OP_INCOMING_STACK_ARG) return true;
    }
    return false;
}

static bool mir_scan_outgoing_stack_args(arm64_mir_emit_t *emit)
{
    int outgoing_size = 0;

    for (size_t i = 0; i < anvil_mir_num_instrs(emit->mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(emit->mir, i, &info)) return false;
        if (info.op != ANVIL_MIR_OP_CALL_STACK_ARG) continue;
        if (!info.has_imm || info.imm < 0 || info.num_uses != 1) return false;
        if (info.imm > INT32_MAX - 8) return false;

        anvil_mir_vreg_t arg = anvil_mir_get_instr_use(emit->mir, i, 0);
        const anvil_mir_vreg_info_t *arg_info =
            anvil_mir_get_vreg_info(emit->mir, arg);
        if (!arg_info) return false;

        int slot_size = align_int(mir_size_bytes(arg_info->size_bits), 8);
        if (slot_size < 8) slot_size = 8;
        int end = (int)info.imm + slot_size;
        if (end > outgoing_size) outgoing_size = end;
    }

    emit->outgoing_size = align_int(outgoing_size, 16);
    return true;
}

static bool mir_prepare_frame(arm64_mir_emit_t *emit)
{
    for (size_t i = 0; i < 32; i++) {
        emit->gpr_save_offsets[i] = -1;
        emit->fpr_save_offsets[i] = -1;
    }

    if (!mir_scan_outgoing_stack_args(emit)) return false;

    int offset = 0;
    for (size_t i = 0; i < anvil_mir_num_vregs(emit->mir); i++) {
        const anvil_regalloc_assignment_t *assignment =
            anvil_mir_get_assignment(emit->mir, (anvil_mir_vreg_t)i);
        if (!assignment || assignment->spilled) continue;

        if (assignment->reg_class == ANVIL_MIR_REG_GPR &&
            assignment->phys_reg >= 19 && assignment->phys_reg <= 28 &&
            emit->gpr_save_offsets[assignment->phys_reg] < 0) {
            offset += 8;
            emit->gpr_save_offsets[assignment->phys_reg] = offset;
        } else if (assignment->reg_class == ANVIL_MIR_REG_FPR &&
                   assignment->phys_reg >= 8 && assignment->phys_reg <= 15 &&
                   emit->fpr_save_offsets[assignment->phys_reg] < 0) {
            offset += 8;
            emit->fpr_save_offsets[assignment->phys_reg] = offset;
        }
    }

    emit->num_spill_offsets = anvil_mir_num_spills(emit->mir);
    emit->num_frame_slot_offsets = anvil_mir_num_frame_slots(emit->mir);
    if (emit->num_frame_slot_offsets > 0) {
        emit->frame_slot_offsets = calloc(emit->num_frame_slot_offsets,
                                          sizeof(*emit->frame_slot_offsets));
        if (!emit->frame_slot_offsets) return false;
    }

    for (size_t i = 0; i < emit->num_frame_slot_offsets; i++) {
        anvil_mir_frame_slot_info_t slot;
        if (!anvil_mir_get_frame_slot_info(emit->mir, (int)i, &slot)) {
            return false;
        }
        int align = slot.align_bytes ? slot.align_bytes : 8;
        if (align > 16) align = 16;
        offset = align_int(offset, align);
        offset += mir_slot_size_bytes(slot.size_bits);
        emit->frame_slot_offsets[i] = offset;
    }

    if (emit->num_spill_offsets > 0) {
        emit->spill_offsets = calloc(emit->num_spill_offsets,
                                     sizeof(*emit->spill_offsets));
        if (!emit->spill_offsets) return false;
    }

    for (size_t i = 0; i < emit->num_spill_offsets; i++) {
        anvil_mir_spill_slot_info_t slot;
        if (!anvil_mir_get_spill_slot_info(emit->mir, (int)i, &slot)) {
            return false;
        }
        offset += align_int(mir_size_bytes(slot.size_bits), 8);
        emit->spill_offsets[i] = offset;
    }

    offset += emit->outgoing_size;
    emit->frame_size = align_int(offset, 16);
    emit->has_frame = emit->frame_size > 0 ||
                      mir_instr_has_call(emit->mir) ||
                      mir_instr_has_dynamic_alloca(emit->mir) ||
                      mir_instr_has_incoming_stack_arg(emit->mir);
    return true;
}

static void mir_emit_variadic_register_save(arm64_mir_emit_t *emit)
{
    for (size_t index = 0; index < anvil_mir_num_instrs(emit->mir); index++)
    {
        anvil_mir_instr_info_t instruction;
        if (!anvil_mir_get_instr_info(emit->mir, index, &instruction))
        {
            emit->failed = true;
            return;
        }
        if (instruction.op != ANVIL_MIR_OP_VA_START)
            continue;

        int offset = emit->frame_slot_offsets[instruction.frame_slot];
        for (unsigned reg = 0; reg < 8; reg++)
        {
            mir_emit_stack_access(&emit->code, "str", arm64_xreg_names[reg], offset - 32 - (int)reg * 8);
            char vector[8];
            snprintf(vector, sizeof(vector), "q%u", reg);
            mir_emit_stack_access(&emit->code, "str", vector, offset - 96 - (int)reg * 16);
        }
    }
}

static void mir_emit_prologue(arm64_mir_emit_t *emit)
{
    const char *name = anvil_mir_func_name(emit->mir);
    const char *prefix = mir_symbol_prefix(emit);
    if (emit->is_darwin) {
        anvil_strbuf_append(&emit->code,
                            "\t.section __TEXT,__text,regular,pure_instructions\n");
        anvil_strbuf_appendf(&emit->code, "\t.globl %s%s\n", prefix, name);
        anvil_strbuf_append(&emit->code, "\t.p2align 2\n");
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    } else {
        anvil_strbuf_append(&emit->code, "\t.text\n");
        anvil_strbuf_appendf(&emit->code, "\t.globl %s\n", name);
        anvil_strbuf_appendf(&emit->code, "\t.type %s, %%function\n", name);
        anvil_strbuf_appendf(&emit->code, "%s:\n", name);
    }

    if (!emit->has_frame) return;

    anvil_strbuf_append(&emit->code, "\tstp x29, x30, [sp, #-16]!\n");
    anvil_strbuf_append(&emit->code, "\tmov x29, sp\n");
    if (emit->frame_size > 0) {
        if (emit->frame_size <= 4095) {
            anvil_strbuf_appendf(&emit->code, "\tsub sp, sp, #%d\n",
                                 emit->frame_size);
        } else {
            anvil_strbuf_appendf(&emit->code, "\tldr x16, =%d\n",
                                 emit->frame_size);
            anvil_strbuf_append(&emit->code, "\tsub sp, sp, x16\n");
        }
    }

    mir_emit_variadic_register_save(emit);

    for (int reg = 19; reg <= 28; reg++) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            mir_emit_stack_access(&emit->code, "str", arm64_xreg_names[reg],
                                  emit->gpr_save_offsets[reg]);
        }
    }
    for (int reg = 8; reg <= 15; reg++) {
        if (emit->fpr_save_offsets[reg] >= 0) {
            mir_emit_stack_access(&emit->code, "str", arm64_dreg_names[reg],
                                  emit->fpr_save_offsets[reg]);
        }
    }
}

static void mir_emit_epilogue(arm64_mir_emit_t *emit)
{
    if (emit->has_frame) {
        for (int reg = 8; reg <= 15; reg++) {
            if (emit->fpr_save_offsets[reg] >= 0) {
                mir_emit_stack_access(&emit->code, "ldr", arm64_dreg_names[reg],
                                      emit->fpr_save_offsets[reg]);
            }
        }
        for (int reg = 28; reg >= 19; reg--) {
            if (emit->gpr_save_offsets[reg] >= 0) {
                mir_emit_stack_access(&emit->code, "ldr", arm64_xreg_names[reg],
                                      emit->gpr_save_offsets[reg]);
            }
        }
        anvil_strbuf_append(&emit->code, "\tmov sp, x29\n");
        anvil_strbuf_append(&emit->code, "\tldp x29, x30, [sp], #16\n");
    }
    anvil_strbuf_append(&emit->code, "\tret\n");
}

static void mir_emit_copy(arm64_mir_emit_t *emit,
                          anvil_mir_vreg_t dst,
                          anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *dst_info = mir_vreg_info_checked(emit, dst);
    const anvil_mir_vreg_info_t *src_info = mir_vreg_info_checked(emit, src);
    const anvil_regalloc_assignment_t *dst_assignment =
        mir_assignment_checked(emit, dst);
    const anvil_regalloc_assignment_t *src_assignment =
        mir_assignment_checked(emit, src);
    if (!dst_info || !src_info || !dst_assignment || !src_assignment) return;

    const char *dst_reg = mir_reg_name(emit, dst);
    const char *src_reg = mir_reg_name(emit, src);
    if (emit->failed) return;
    if (dst_assignment->phys_reg == src_assignment->phys_reg &&
        dst_info->reg_class == src_info->reg_class) {
        return;
    }

    int size = mir_size_bytes(dst_info->size_bits);
    if (dst_info->reg_class == ANVIL_MIR_REG_FPR &&
        src_info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\tfmov %s, %s\n", dst_reg, src_reg);
    } else if (dst_info->reg_class == ANVIL_MIR_REG_GPR &&
               src_info->reg_class == ANVIL_MIR_REG_GPR) {
        const char *source = mir_gpr_name_for_size(src_assignment->phys_reg, size);
        anvil_strbuf_appendf(&emit->code, "\tmov %s, %s\n", dst_reg, source);
    } else if (dst_info->reg_class == ANVIL_MIR_REG_FPR &&
               src_info->reg_class == ANVIL_MIR_REG_GPR) {
        anvil_strbuf_appendf(&emit->code, "\tfmov %s, %s\n", dst_reg, src_reg);
    } else if (dst_info->reg_class == ANVIL_MIR_REG_GPR &&
               src_info->reg_class == ANVIL_MIR_REG_FPR) {
        (void)size;
        anvil_strbuf_appendf(&emit->code, "\tfmov %s, %s\n", dst_reg, src_reg);
    } else {
        emit->failed = true;
    }
}

static bool mir_get_uses2(const anvil_mir_func_t *mir,
                          size_t instr_index,
                          anvil_mir_vreg_t *lhs,
                          anvil_mir_vreg_t *rhs)
{
    *lhs = anvil_mir_get_instr_use(mir, instr_index, 0);
    *rhs = anvil_mir_get_instr_use(mir, instr_index, 1);
    return *lhs != ANVIL_MIR_NO_VREG && *rhs != ANVIL_MIR_NO_VREG;
}

static void mir_emit_binary(arm64_mir_emit_t *emit,
                            size_t instr_index,
                            const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs;
    anvil_mir_vreg_t rhs;
    if (!mir_get_uses2(emit->mir, instr_index, &lhs, &rhs)) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = mir_vreg_info_checked(emit, info->def);
    if (!def_info) return;

    const char *dst = mir_reg_name(emit, info->def);
    const char *a = mir_reg_name(emit, lhs);
    const char *b = mir_reg_name(emit, rhs);
    if (emit->failed) return;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        const char *op = NULL;
        switch (info->op) {
            case ANVIL_MIR_OP_ADD: op = "fadd"; break;
            case ANVIL_MIR_OP_SUB: op = "fsub"; break;
            case ANVIL_MIR_OP_MUL: op = "fmul"; break;
            case ANVIL_MIR_OP_DIV:
            case ANVIL_MIR_OP_FDIV: op = "fdiv"; break;
            default: break;
        }
        if (!op) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", op, dst, a, b);
        return;
    }

    switch (info->op) {
        case ANVIL_MIR_OP_ADD:
            anvil_strbuf_appendf(&emit->code, "\tadd %s, %s, %s\n", dst, a, b);
            break;
        case ANVIL_MIR_OP_SUB:
            anvil_strbuf_appendf(&emit->code, "\tsub %s, %s, %s\n", dst, a, b);
            break;
        case ANVIL_MIR_OP_MUL:
            anvil_strbuf_appendf(&emit->code, "\tmul %s, %s, %s\n", dst, a, b);
            break;
        case ANVIL_MIR_OP_DIV:
        case ANVIL_MIR_OP_SDIV:
            anvil_strbuf_appendf(&emit->code, "\tsdiv %s, %s, %s\n", dst, a, b);
            break;
        case ANVIL_MIR_OP_UDIV:
            anvil_strbuf_appendf(&emit->code, "\tudiv %s, %s, %s\n", dst, a, b);
            break;
        case ANVIL_MIR_OP_SMOD: {
            const anvil_mir_vreg_info_t *dst_info = mir_vreg_info_checked(emit, info->def);
            const char *tmp = mir_gpr_scratch_name(dst_info ? dst_info->size_bits : 64, 17);
            anvil_strbuf_appendf(&emit->code, "\tsdiv %s, %s, %s\n", tmp, a, b);
            anvil_strbuf_appendf(&emit->code, "\tmsub %s, %s, %s, %s\n",
                                 dst, tmp, b, a);
            break;
        }
        case ANVIL_MIR_OP_UMOD: {
            const anvil_mir_vreg_info_t *dst_info = mir_vreg_info_checked(emit, info->def);
            const char *tmp = mir_gpr_scratch_name(dst_info ? dst_info->size_bits : 64, 17);
            anvil_strbuf_appendf(&emit->code, "\tudiv %s, %s, %s\n", tmp, a, b);
            anvil_strbuf_appendf(&emit->code, "\tmsub %s, %s, %s, %s\n",
                                 dst, tmp, b, a);
            break;
        }
        case ANVIL_MIR_OP_AND:
            anvil_strbuf_appendf(&emit->code, "\tand %s, %s, %s\n", dst, a, b);
            break;
        case ANVIL_MIR_OP_OR:
            anvil_strbuf_appendf(&emit->code, "\torr %s, %s, %s\n", dst, a, b);
            break;
        case ANVIL_MIR_OP_XOR:
            anvil_strbuf_appendf(&emit->code, "\teor %s, %s, %s\n", dst, a, b);
            break;
        case ANVIL_MIR_OP_SHL:
            anvil_strbuf_appendf(&emit->code, "\tlsl %s, %s, %s\n", dst, a, b);
            break;
        case ANVIL_MIR_OP_SHR:
            anvil_strbuf_appendf(&emit->code, "\tlsr %s, %s, %s\n", dst, a, b);
            break;
        case ANVIL_MIR_OP_SAR:
            anvil_strbuf_appendf(&emit->code, "\tasr %s, %s, %s\n", dst, a, b);
            break;
        default:
            emit->failed = true;
            break;
    }
}

static void mir_emit_cmp(arm64_mir_emit_t *emit,
                         size_t instr_index,
                         const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs;
    anvil_mir_vreg_t rhs;
    if (!mir_get_uses2(emit->mir, instr_index, &lhs, &rhs)) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *lhs_info = mir_vreg_info_checked(emit, lhs);
    const char *a = mir_reg_name(emit, lhs);
    const char *b = mir_reg_name(emit, rhs);
    const char *dst = mir_reg_name(emit, info->def);
    if (!lhs_info || emit->failed) return;

    if (lhs_info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\tfcmp %s, %s\n", a, b);
        if (info->op == ANVIL_MIR_OP_FCMP) {
            anvil_fcmp_pred_t pred = (anvil_fcmp_pred_t)info->imm;
            static const unsigned masks[] = {
                0,4,2,6,1,5,3,7,12,10,14,9,13,11,8,15
            };
            unsigned mask = masks[pred];
            const char *name = anvil_mir_func_name(emit->mir);
            anvil_strbuf_appendf(&emit->code, "\tmov %s, #0\n", dst);
            if (mask == 15) anvil_strbuf_appendf(&emit->code, "\tmov %s, #1\n", dst);
            else if (mask != 0) {
                if (mask & 8) anvil_strbuf_appendf(&emit->code,
                    "\tb.vs .L%s_fcmp_true_%zu\n", name, instr_index);
                else anvil_strbuf_appendf(&emit->code,
                    "\tb.vs .L%s_fcmp_done_%zu\n", name, instr_index);
                if (mask & 1) anvil_strbuf_appendf(&emit->code,
                    "\tb.mi .L%s_fcmp_true_%zu\n", name, instr_index);
                if (mask & 2) anvil_strbuf_appendf(&emit->code,
                    "\tb.gt .L%s_fcmp_true_%zu\n", name, instr_index);
                if (mask & 4) anvil_strbuf_appendf(&emit->code,
                    "\tb.eq .L%s_fcmp_true_%zu\n", name, instr_index);
                anvil_strbuf_appendf(&emit->code,
                    "\tb .L%s_fcmp_done_%zu\n.L%s_fcmp_true_%zu:\n"
                    "\tmov %s, #1\n.L%s_fcmp_done_%zu:\n",
                    name, instr_index, name, instr_index, dst,
                    name, instr_index);
            }
            return;
        }
    } else {
        anvil_strbuf_appendf(&emit->code, "\tcmp %s, %s\n", a, b);
    }
    anvil_strbuf_appendf(&emit->code, "\tcset %s, %s\n",
                         dst, mir_cmp_cond(info->op));
}

static void mir_emit_unary(arm64_mir_emit_t *emit,
                           size_t instr_index,
                           const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = mir_vreg_info_checked(emit, info->def);
    const char *dst = mir_reg_name(emit, info->def);
    const char *s = mir_reg_name(emit, src);
    if (!def_info || emit->failed) return;

    if (info->op == ANVIL_MIR_OP_NEG) {
        if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
            anvil_strbuf_appendf(&emit->code, "\tfneg %s, %s\n", dst, s);
        } else {
            anvil_strbuf_appendf(&emit->code, "\tneg %s, %s\n", dst, s);
        }
    } else if (info->op == ANVIL_MIR_OP_FABS &&
               def_info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\tfabs %s, %s\n", dst, s);
    } else if (info->op == ANVIL_MIR_OP_NOT &&
               def_info->reg_class == ANVIL_MIR_REG_GPR) {
        anvil_strbuf_appendf(&emit->code, "\tmvn %s, %s\n", dst, s);
    } else {
        emit->failed = true;
    }
}

static void mir_emit_frame_addr(arm64_mir_emit_t *emit,
                                const anvil_mir_instr_info_t *info)
{
    if (info->frame_slot < 0 ||
        (size_t)info->frame_slot >= emit->num_frame_slot_offsets) {
        emit->failed = true;
        return;
    }

    const char *dst = mir_reg_name(emit, info->def);
    if (emit->failed) return;

    int offset = emit->frame_slot_offsets[info->frame_slot];
    if (offset <= 4095) {
        anvil_strbuf_appendf(&emit->code, "\tsub %s, x29, #%d\n", dst, offset);
    } else {
        anvil_strbuf_appendf(&emit->code, "\tldr x16, =%d\n", offset);
        anvil_strbuf_appendf(&emit->code, "\tsub %s, x29, x16\n", dst);
    }
}

static void mir_emit_dyn_alloca(arm64_mir_emit_t *emit,
                                size_t instr_index,
                                const anvil_mir_instr_info_t *info)
{
    if (!info->has_imm || info->imm <= 0 || info->num_uses != 1) {
        emit->failed = true;
        return;
    }

    anvil_mir_vreg_t count = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (count == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const char *dst = mir_reg_name(emit, info->def);
    const char *count_reg = mir_reg_name(emit, count);
    if (emit->failed) return;

    if (info->imm == 1) {
        anvil_strbuf_appendf(&emit->code, "\tmov x16, %s\n", count_reg);
    } else {
        mir_emit_mov_gpr_imm(&emit->code, "x17", info->imm);
        anvil_strbuf_appendf(&emit->code, "\tmul x16, %s, x17\n", count_reg);
    }
    anvil_strbuf_append(&emit->code, "\tadd x16, x16, #15\n");
    anvil_strbuf_append(&emit->code, "\tand x16, x16, #0xfffffffffffffff0\n");
    anvil_strbuf_append(&emit->code, "\tsub sp, sp, x16\n");
    anvil_strbuf_appendf(&emit->code, "\tmov %s, sp\n", dst);
}

static void mir_emit_mov(arm64_mir_emit_t *emit,
                         const anvil_mir_instr_info_t *info)
{
    const anvil_mir_vreg_info_t *def_info = mir_vreg_info_checked(emit, info->def);
    const char *dst = mir_reg_name(emit, info->def);
    if (!def_info || emit->failed) return;

    if (!info->has_imm) {
        anvil_strbuf_appendf(&emit->code, "\tmov %s, #0\n", dst);
        return;
    }

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = mir_size_bytes(def_info->size_bits);
        if (size <= 4) {
            anvil_strbuf_appendf(&emit->code, "\tldr w16, =0x%08x\n",
                                 (uint32_t)info->imm);
            anvil_strbuf_appendf(&emit->code, "\tfmov %s, w16\n", dst);
        } else {
            anvil_strbuf_appendf(&emit->code, "\tldr x16, =0x%016llx\n",
                                 (unsigned long long)(uint64_t)info->imm);
            anvil_strbuf_appendf(&emit->code, "\tfmov %s, x16\n", dst);
        }
    } else {
        mir_emit_mov_gpr_imm(&emit->code, dst, info->imm);
    }
}

static void mir_emit_symbol_addr(arm64_mir_emit_t *emit,
                                 const anvil_mir_instr_info_t *info)
{
    if (!info->symbol || !info->symbol[0]) {
        emit->failed = true;
        return;
    }

    const char *dst = mir_reg_name(emit, info->def);
    if (emit->failed) return;

    const char *prefix = mir_symbol_ref_prefix(emit, info->symbol);
    if (emit->is_darwin) {
        anvil_strbuf_appendf(&emit->code, "\tadrp %s, %s%s",
                             dst, prefix, info->symbol);
        if (info->has_imm && info->imm != 0)
            anvil_strbuf_appendf(&emit->code, "%+lld", (long long)info->imm);
        anvil_strbuf_append(&emit->code, "@PAGE\n");
        anvil_strbuf_appendf(&emit->code, "\tadd %s, %s, %s%s",
                             dst, dst, prefix, info->symbol);
        if (info->has_imm && info->imm != 0)
            anvil_strbuf_appendf(&emit->code, "%+lld", (long long)info->imm);
        anvil_strbuf_append(&emit->code, "@PAGEOFF\n");
    } else {
        anvil_strbuf_appendf(&emit->code, "\tadrp %s, %s%s",
                             dst, prefix, info->symbol);
        if (info->has_imm && info->imm != 0)
            anvil_strbuf_appendf(&emit->code, "%+lld", (long long)info->imm);
        anvil_strbuf_append(&emit->code, "\n");
        anvil_strbuf_appendf(&emit->code, "\tadd %s, %s, :lo12:%s%s",
                             dst, dst, prefix, info->symbol);
        if (info->has_imm && info->imm != 0)
            anvil_strbuf_appendf(&emit->code, "%+lld", (long long)info->imm);
        anvil_strbuf_append(&emit->code, "\n");
    }
}

static void mir_emit_load(arm64_mir_emit_t *emit,
                          size_t instr_index,
                          const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    const anvil_mir_vreg_info_t *def_info = mir_vreg_info_checked(emit, info->def);
    const anvil_regalloc_assignment_t *def_assignment =
        mir_assignment_checked(emit, info->def);
    const char *dst = mir_reg_name(emit, info->def);
    const char *base = mir_reg_name(emit, ptr);
    if (!def_info || !def_assignment || emit->failed) return;

    int size = mir_size_bytes(def_info->size_bits);
    if (def_info->reg_class == ANVIL_MIR_REG_GPR &&
        def_info->is_signed && size == 4) {
        dst = arm64_xreg_names[def_assignment->phys_reg];
    }
    mir_emit_base_offset_access(&emit->code,
                                mir_load_op(def_info->reg_class, size,
                                            def_info->is_signed),
                                dst, base, size,
                                info->has_imm ? info->imm : 0);
}

static void mir_emit_store(arm64_mir_emit_t *emit, size_t instr_index)
{
    anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (value == ANVIL_MIR_NO_VREG || ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    const anvil_mir_vreg_info_t *value_info = mir_vreg_info_checked(emit, value);
    const char *src = mir_reg_name(emit, value);
    const char *base = mir_reg_name(emit, ptr);
    if (!value_info || emit->failed) return;

    int size = mir_size_bytes(value_info->size_bits);
    anvil_mir_instr_info_t info;
    int64_t offset = 0;
    if (anvil_mir_get_instr_info(emit->mir, instr_index, &info) &&
        info.has_imm) {
        offset = info.imm;
    }
    mir_emit_base_offset_access(&emit->code,
                                mir_store_op(value_info->reg_class, size),
                                src, base, size, offset);
}

static void mir_emit_incoming_stack_arg(arm64_mir_emit_t *emit,
                                        const anvil_mir_instr_info_t *info)
{
    if (!emit->has_frame || !info->has_imm || info->imm < 0 ||
        info->imm > INT32_MAX - 16) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = mir_vreg_info_checked(emit, info->def);
    const anvil_regalloc_assignment_t *def_assignment =
        mir_assignment_checked(emit, info->def);
    const char *dst = mir_reg_name(emit, info->def);
    if (!def_info || !def_assignment || emit->failed) return;

    int size = mir_size_bytes(def_info->size_bits);
    if (def_info->reg_class == ANVIL_MIR_REG_GPR &&
        def_info->is_signed && size == 4) {
        dst = arm64_xreg_names[def_assignment->phys_reg];
    }

    int frame_offset = 16 + (int)info->imm;
    mir_emit_incoming_stack_access(
        &emit->code,
        mir_load_op(def_info->reg_class, size, def_info->is_signed),
        dst,
        frame_offset);
}

static void mir_emit_gpr_extend(arm64_mir_emit_t *emit,
                                size_t instr_index,
                                const anvil_mir_instr_info_t *info,
                                bool sign_extend)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = mir_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = mir_vreg_info_checked(emit, src);
    const anvil_regalloc_assignment_t *dst_assignment =
        mir_assignment_checked(emit, info->def);
    const anvil_regalloc_assignment_t *src_assignment =
        mir_assignment_checked(emit, src);
    if (!dst_info || !src_info || !dst_assignment || !src_assignment) return;
    if (dst_info->reg_class != ANVIL_MIR_REG_GPR ||
        src_info->reg_class != ANVIL_MIR_REG_GPR) {
        emit->failed = true;
        return;
    }

    int src_size = mir_size_bytes(src_info->size_bits);
    int dst_size = mir_size_bytes(dst_info->size_bits);
    const char *dst_w = arm64_wreg_names[dst_assignment->phys_reg];
    const char *src_w = arm64_wreg_names[src_assignment->phys_reg];
    const char *dst_reg = mir_gpr_name_for_size(dst_assignment->phys_reg, dst_size);

    if (sign_extend) {
        if (src_size <= 1) {
            anvil_strbuf_appendf(&emit->code, "\tsxtb %s, %s\n", dst_reg, src_w);
        } else if (src_size <= 2) {
            anvil_strbuf_appendf(&emit->code, "\tsxth %s, %s\n", dst_reg, src_w);
        } else if (dst_size > 4 && src_size <= 4) {
            anvil_strbuf_appendf(&emit->code, "\tsxtw %s, %s\n", dst_reg, src_w);
        } else {
            anvil_strbuf_appendf(&emit->code, "\tmov %s, %s\n",
                                 dst_reg,
                                 mir_gpr_name_for_size(src_assignment->phys_reg, dst_size));
        }
        return;
    }

    if (src_size <= 1) {
        anvil_strbuf_appendf(&emit->code, "\tuxtb %s, %s\n", dst_w, src_w);
    } else if (src_size <= 2) {
        anvil_strbuf_appendf(&emit->code, "\tuxth %s, %s\n", dst_w, src_w);
    } else {
        anvil_strbuf_appendf(&emit->code, "\tmov %s, %s\n", dst_w, src_w);
    }
}

static void mir_emit_cast(arm64_mir_emit_t *emit,
                          size_t instr_index,
                          const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = mir_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = mir_vreg_info_checked(emit, src);
    if (!dst_info || !src_info) return;

    switch (info->op) {
        case ANVIL_MIR_OP_ZEXT:
            mir_emit_gpr_extend(emit, instr_index, info, false);
            break;
        case ANVIL_MIR_OP_SEXT:
            mir_emit_gpr_extend(emit, instr_index, info, true);
            break;
        case ANVIL_MIR_OP_TRUNC:
        case ANVIL_MIR_OP_BITCAST:
            mir_emit_copy(emit, info->def, src);
            break;
        case ANVIL_MIR_OP_SITOFP:
        case ANVIL_MIR_OP_UITOFP: {
            if (dst_info->reg_class != ANVIL_MIR_REG_FPR ||
                src_info->reg_class != ANVIL_MIR_REG_GPR) {
                emit->failed = true;
                return;
            }
            const char *dst = mir_reg_name(emit, info->def);
            const char *src_reg = mir_reg_name(emit, src);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\t%s %s, %s\n",
                                 info->op == ANVIL_MIR_OP_SITOFP ? "scvtf" : "ucvtf",
                                 dst, src_reg);
            break;
        }
        case ANVIL_MIR_OP_FPTOSI:
        case ANVIL_MIR_OP_FPTOUI: {
            if (dst_info->reg_class != ANVIL_MIR_REG_GPR ||
                src_info->reg_class != ANVIL_MIR_REG_FPR) {
                emit->failed = true;
                return;
            }
            const char *dst = mir_reg_name(emit, info->def);
            const char *src_reg = mir_reg_name(emit, src);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\t%s %s, %s\n",
                                 info->op == ANVIL_MIR_OP_FPTOSI ? "fcvtzs" : "fcvtzu",
                                 dst, src_reg);
            break;
        }
        case ANVIL_MIR_OP_FPEXT:
        case ANVIL_MIR_OP_FPTRUNC: {
            if (dst_info->reg_class != ANVIL_MIR_REG_FPR ||
                src_info->reg_class != ANVIL_MIR_REG_FPR) {
                emit->failed = true;
                return;
            }
            const char *dst = mir_reg_name(emit, info->def);
            const char *src_reg = mir_reg_name(emit, src);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\tfcvt %s, %s\n", dst, src_reg);
            break;
        }
        default:
            emit->failed = true;
            break;
    }
}

static void mir_emit_select(arm64_mir_emit_t *emit,
                            size_t instr_index,
                            const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t cond = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t then_v = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    anvil_mir_vreg_t else_v = anvil_mir_get_instr_use(emit->mir, instr_index, 2);
    if (cond == ANVIL_MIR_NO_VREG ||
        then_v == ANVIL_MIR_NO_VREG ||
        else_v == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = mir_vreg_info_checked(emit, info->def);
    const char *cond_reg = mir_reg_name(emit, cond);
    const char *dst = mir_reg_name(emit, info->def);
    const char *then_reg = mir_reg_name(emit, then_v);
    const char *else_reg = mir_reg_name(emit, else_v);
    if (!def_info || emit->failed) return;

    anvil_strbuf_appendf(&emit->code, "\tcmp %s, #0\n", cond_reg);
    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\tfcsel %s, %s, %s, ne\n",
                             dst, then_reg, else_reg);
    } else {
        anvil_strbuf_appendf(&emit->code, "\tcsel %s, %s, %s, ne\n",
                             dst, then_reg, else_reg);
    }
}

static void mir_emit_call_stack_arg(arm64_mir_emit_t *emit,
                                    size_t instr_index,
                                    const anvil_mir_instr_info_t *info)
{
    if (!info->has_imm || info->imm < 0 || info->num_uses != 1 ||
        info->imm > INT32_MAX) {
        emit->failed = true;
        return;
    }

    anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (value == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *value_info =
        mir_vreg_info_checked(emit, value);
    const char *src = mir_reg_name(emit, value);
    if (!value_info || emit->failed) return;

    int size = mir_size_bytes(value_info->size_bits);
    mir_emit_sp_access(&emit->code,
                       mir_store_op(value_info->reg_class, size),
                       src, (int)info->imm);
}

static void mir_emit_spill_load(arm64_mir_emit_t *emit,
                                const anvil_mir_instr_info_t *info)
{
    if (info->spill_slot < 0 ||
        (size_t)info->spill_slot >= emit->num_spill_offsets) {
        emit->failed = true;
        return;
    }

    anvil_mir_spill_slot_info_t slot;
    if (!anvil_mir_get_spill_slot_info(emit->mir, info->spill_slot, &slot)) {
        emit->failed = true;
        return;
    }

    const char *dst = mir_reg_name(emit, info->def);
    if (emit->failed) return;
    int size = mir_size_bytes(slot.size_bits);
    mir_emit_stack_access(&emit->code, mir_load_op(slot.reg_class, size, false), dst,
                          emit->spill_offsets[info->spill_slot]);
}

static void mir_emit_spill_store(arm64_mir_emit_t *emit,
                                 size_t instr_index,
                                 const anvil_mir_instr_info_t *info)
{
    if (info->spill_slot < 0 ||
        (size_t)info->spill_slot >= emit->num_spill_offsets) {
        emit->failed = true;
        return;
    }

    anvil_mir_vreg_t src_vreg = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src_vreg == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    anvil_mir_spill_slot_info_t slot;
    if (!anvil_mir_get_spill_slot_info(emit->mir, info->spill_slot, &slot)) {
        emit->failed = true;
        return;
    }

    const char *src = mir_reg_name(emit, src_vreg);
    if (emit->failed) return;
    int size = mir_size_bytes(slot.size_bits);
    mir_emit_stack_access(&emit->code, mir_store_op(slot.reg_class, size), src,
                          emit->spill_offsets[info->spill_slot]);
}

static void mir_emit_ret(arm64_mir_emit_t *emit,
                         size_t instr_index,
                         const anvil_mir_instr_info_t *info)
{
    if (info->num_uses > 0) {
        anvil_mir_vreg_t ret = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        const anvil_mir_vreg_info_t *ret_info = mir_vreg_info_checked(emit, ret);
        const anvil_regalloc_assignment_t *assignment =
            mir_assignment_checked(emit, ret);
        if (!ret_info || !assignment) return;

        if (ret_info->reg_class == ANVIL_MIR_REG_FPR) {
            const char *src = mir_reg_name(emit, ret);
            const char *dst = mir_size_bytes(ret_info->size_bits) <= 4 ? "s0" : "d0";
            if (assignment->phys_reg != 0) {
                anvil_strbuf_appendf(&emit->code, "\tfmov %s, %s\n", dst, src);
            }
        } else {
            const char *src = mir_reg_name(emit, ret);
            const char *dst = mir_size_bytes(ret_info->size_bits) <= 4 ? "w0" : "x0";
            if (assignment->phys_reg != 0) {
                anvil_strbuf_appendf(&emit->code, "\tmov %s, %s\n", dst, src);
            }
        }
    }
    if (!emit->failed) mir_emit_epilogue(emit);
}

static void mir_emit_va_start(arm64_mir_emit_t *emit, const anvil_mir_instr_info_t *instruction)
{
    if (emit->is_darwin)
    {
        emit->failed = true;
        return;
    }

    int offset = emit->frame_slot_offsets[instruction->frame_slot];
    mir_emit_mov_gpr_imm(&emit->code, "x16", offset);
    anvil_strbuf_append(&emit->code, "\tsub x17, x29, x16\n");
    mir_emit_mov_gpr_imm(&emit->code, "x16", (int64_t)instruction->named_stack_bytes + 16);
    anvil_strbuf_append(&emit->code, "\tadd x16, x29, x16\n\tstr x16, [x17]\n");
    anvil_strbuf_append(&emit->code, "\tadd x16, x17, #96\n\tstr x16, [x17, #8]\n");
    anvil_strbuf_append(&emit->code, "\tadd x16, x17, #224\n\tstr x16, [x17, #16]\n");
    mir_emit_mov_gpr_imm(&emit->code, "w16", -(int64_t)(8 - instruction->named_gpr) * 8);
    anvil_strbuf_append(&emit->code, "\tstr w16, [x17, #24]\n");
    mir_emit_mov_gpr_imm(&emit->code, "w16", -(int64_t)(8 - instruction->named_fpr) * 16);
    anvil_strbuf_append(&emit->code, "\tstr w16, [x17, #28]\n");
    const char *destination = mir_reg_name(emit, instruction->def);
    if (!emit->failed)
        anvil_strbuf_appendf(&emit->code, "\tmov %s, x17\n", destination);
}

static void mir_emit_atomic(arm64_mir_emit_t *emit, size_t index, const anvil_mir_instr_info_t *instruction)
{
    if (instruction->atomic_op == ANVIL_OP_ATOMIC_FENCE)
    {
        if (instruction->atomic.order != ANVIL_ORDER_RELAXED)
            anvil_strbuf_append(&emit->code, "\tdmb ish\n");

        return;
    }

    anvil_mir_vreg_t pointer = anvil_mir_get_instr_use(emit->mir, index, 0);
    anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, index, 1);
    anvil_mir_vreg_t typed = instruction->def != ANVIL_MIR_NO_VREG ? instruction->def : value;
    const anvil_mir_vreg_info_t *type = mir_vreg_info_checked(emit, typed);
    const char *address = mir_reg_name(emit, pointer);
    const char *operand = instruction->num_uses > 1 ? mir_reg_name(emit, value) : NULL;
    if (!type || emit->failed)
        return;

    unsigned bits = type->size_bits;
    const char *suffix = bits == 8 ? "b" : (bits == 16 ? "h" : "");
    const char *old = bits == 64 ? "x9" : "w9";
    const char *temporary = bits == 64 ? "x10" : "w10";
    bool acquire = instruction->atomic.order == ANVIL_ORDER_ACQUIRE || instruction->atomic.order == ANVIL_ORDER_ACQ_REL ||
                   instruction->atomic.order == ANVIL_ORDER_SEQ_CST;
    bool release = instruction->atomic.order == ANVIL_ORDER_RELEASE || instruction->atomic.order == ANVIL_ORDER_ACQ_REL ||
                   instruction->atomic.order == ANVIL_ORDER_SEQ_CST;

    if (instruction->atomic_op == ANVIL_OP_ATOMIC_LOAD)
    {
        anvil_strbuf_appendf(&emit->code, "\t%s%s %s, [%s]\n", acquire ? "ldar" : "ldr", suffix, old, address);
    }
    else if (instruction->atomic_op == ANVIL_OP_ATOMIC_STORE)
    {
        anvil_strbuf_appendf(&emit->code, "\t%s%s %s, [%s]\n", release ? "stlr" : "str", suffix, operand, address);
        return;
    }
    else
    {
        const char *name = anvil_mir_func_name(emit->mir);
        bool compare = instruction->atomic_op == ANVIL_OP_ATOMIC_CMPXCHG;
        if (compare)
        {
            if (bits < 32)
                anvil_strbuf_appendf(&emit->code, "\t%s w10, %s\n", bits == 8 ? "uxtb" : "uxth", operand);
            else
                anvil_strbuf_appendf(&emit->code, "\tmov %s, %s\n", temporary, operand);

            operand = mir_reg_name(emit, anvil_mir_get_instr_use(emit->mir, index, 2));
        }

        anvil_strbuf_appendf(&emit->code, ".L%s_atomic_%zu_retry:\n\t%s%s %s, [%s]\n", name, index, acquire ? "ldaxr" : "ldxr", suffix, old, address);
        const char *replacement = operand;
        if (compare)
        {
            anvil_strbuf_appendf(&emit->code, "\tcmp %s, %s\n\tb.ne .L%s_atomic_%zu_mismatch\n", old, temporary, name, index);
        }
        else if (instruction->atomic.rmw != ANVIL_ATOMIC_EXCHANGE)
        {
            static const char *operations[] = { NULL, "add", "sub", "and", "orr", "eor" };
            anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", operations[instruction->atomic.rmw], temporary, old, operand);
            replacement = temporary;
        }

        /* The status register is distinct from the address and stored value;
         * retrying the exclusive pair implements strong compare/exchange. */
        anvil_strbuf_appendf(&emit->code, "\t%s%s w11, %s, [%s]\n\tcbnz w11, .L%s_atomic_%zu_retry\n",
                             release ? "stlxr" : "stxr", suffix, replacement, address, name, index);
        if (compare)
        {
            anvil_strbuf_appendf(&emit->code, "\tb .L%s_atomic_%zu_done\n.L%s_atomic_%zu_mismatch:\n\tclrex\n.L%s_atomic_%zu_done:\n",
                                 name, index, name, index, name, index);
        }
    }

    const char *destination = mir_reg_name(emit, instruction->def);
    if (!emit->failed)
        anvil_strbuf_appendf(&emit->code, "\tmov %s, %s\n", destination, old);
}

static void mir_emit_instr(arm64_mir_emit_t *emit,
                           size_t instr_index,
                           const anvil_mir_instr_info_t *info)
{
    switch (info->op) {
        case ANVIL_MIR_OP_VA_START:
            mir_emit_va_start(emit, info);
            break;

        case ANVIL_MIR_OP_ATOMIC:
            mir_emit_atomic(emit, instr_index, info);
            break;

        case ANVIL_MIR_OP_MOV:
            mir_emit_mov(emit, info);
            break;
        case ANVIL_MIR_OP_COPY: {
            anvil_mir_vreg_t src =
                anvil_mir_get_instr_use(emit->mir, instr_index, 0);
            if (src == ANVIL_MIR_NO_VREG) {
                emit->failed = true;
                break;
            }
            mir_emit_copy(emit, info->def, src);
            break;
        }
        case ANVIL_MIR_OP_ADD:
        case ANVIL_MIR_OP_SUB:
        case ANVIL_MIR_OP_MUL:
        case ANVIL_MIR_OP_DIV:
        case ANVIL_MIR_OP_SDIV:
        case ANVIL_MIR_OP_UDIV:
        case ANVIL_MIR_OP_FDIV:
        case ANVIL_MIR_OP_SMOD:
        case ANVIL_MIR_OP_UMOD:
        case ANVIL_MIR_OP_AND:
        case ANVIL_MIR_OP_OR:
        case ANVIL_MIR_OP_XOR:
        case ANVIL_MIR_OP_SHL:
        case ANVIL_MIR_OP_SHR:
        case ANVIL_MIR_OP_SAR:
            mir_emit_binary(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_NEG:
        case ANVIL_MIR_OP_NOT:
        case ANVIL_MIR_OP_FABS:
            mir_emit_unary(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_ZEXT:
        case ANVIL_MIR_OP_SEXT:
        case ANVIL_MIR_OP_TRUNC:
        case ANVIL_MIR_OP_BITCAST:
        case ANVIL_MIR_OP_SITOFP:
        case ANVIL_MIR_OP_UITOFP:
        case ANVIL_MIR_OP_FPTOSI:
        case ANVIL_MIR_OP_FPTOUI:
        case ANVIL_MIR_OP_FPEXT:
        case ANVIL_MIR_OP_FPTRUNC:
            mir_emit_cast(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_SELECT:
            mir_emit_select(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_FCMP:
        case ANVIL_MIR_OP_CMP:
        case ANVIL_MIR_OP_CMP_EQ:
        case ANVIL_MIR_OP_CMP_NE:
        case ANVIL_MIR_OP_CMP_LT:
        case ANVIL_MIR_OP_CMP_LE:
        case ANVIL_MIR_OP_CMP_GT:
        case ANVIL_MIR_OP_CMP_GE:
        case ANVIL_MIR_OP_CMP_ULT:
        case ANVIL_MIR_OP_CMP_ULE:
        case ANVIL_MIR_OP_CMP_UGT:
        case ANVIL_MIR_OP_CMP_UGE:
            mir_emit_cmp(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_SYMBOL_ADDR:
            mir_emit_symbol_addr(emit, info);
            break;
        case ANVIL_MIR_OP_LOAD:
            mir_emit_load(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_STORE:
            mir_emit_store(emit, instr_index);
            break;
        case ANVIL_MIR_OP_FRAME_ADDR:
            mir_emit_frame_addr(emit, info);
            break;
        case ANVIL_MIR_OP_DYN_ALLOCA:
            mir_emit_dyn_alloca(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_INCOMING_STACK_ARG:
            mir_emit_incoming_stack_arg(emit, info);
            break;
        case ANVIL_MIR_OP_CALL:
            if (info->symbol && info->symbol[0]) {
                anvil_strbuf_appendf(&emit->code, "\tbl %s%s\n",
                                     mir_symbol_ref_prefix(emit, info->symbol),
                                     info->symbol);
                break;
            }
            if (info->num_uses == 0) {
                emit->failed = true;
                break;
            }
            anvil_mir_vreg_t target =
                anvil_mir_get_instr_use(emit->mir, instr_index, 0);
            if (target == ANVIL_MIR_NO_VREG) {
                emit->failed = true;
                break;
            }
            anvil_strbuf_appendf(&emit->code, "\tblr %s\n",
                                 mir_reg_name(emit, target));
            break;
        case ANVIL_MIR_OP_CALL_STACK_ARG:
            mir_emit_call_stack_arg(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_BR:
            anvil_strbuf_append(&emit->code, "\tb ");
            if (!mir_emit_branch_target(emit, info->true_block)) {
                emit->failed = true;
                break;
            }
            anvil_strbuf_append(&emit->code, "\n");
            break;
        case ANVIL_MIR_OP_BR_COND: {
            anvil_mir_vreg_t cond =
                anvil_mir_get_instr_use(emit->mir, instr_index, 0);
            if (cond == ANVIL_MIR_NO_VREG) {
                emit->failed = true;
                break;
            }
            const char *cond_reg = mir_reg_name(emit, cond);
            if (emit->failed) break;
            anvil_strbuf_appendf(&emit->code, "\tcbnz %s, ", cond_reg);
            if (!mir_emit_branch_target(emit, info->true_block)) {
                emit->failed = true;
                break;
            }
            anvil_strbuf_append(&emit->code, "\n\tb ");
            if (!mir_emit_branch_target(emit, info->false_block)) {
                emit->failed = true;
                break;
            }
            anvil_strbuf_append(&emit->code, "\n");
            break;
        }
        case ANVIL_MIR_OP_RET:
            mir_emit_ret(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_SPILL_LOAD:
            mir_emit_spill_load(emit, info);
            break;
        case ANVIL_MIR_OP_SPILL_STORE:
            mir_emit_spill_store(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_KEEPALIVE:
            break;
        case ANVIL_MIR_OP_CALL_RESULT:
        case ANVIL_MIR_OP_RET_VALUE_PART:
            emit->failed = true;
            break;
        default:
            emit->failed = true;
            break;
    }
}

static void mir_emit_escaped_string(anvil_strbuf_t *code, const char *value)
{
    anvil_strbuf_append(code, "\t.asciz \"");
    for (const char *p = value ? value : ""; *p; p++) {
        switch (*p) {
            case '\n':
                anvil_strbuf_append(code, "\\n");
                break;
            case '\r':
                anvil_strbuf_append(code, "\\r");
                break;
            case '\t':
                anvil_strbuf_append(code, "\\t");
                break;
            case '\\':
                anvil_strbuf_append(code, "\\\\");
                break;
            case '"':
                anvil_strbuf_append(code, "\\\"");
                break;
            default:
                anvil_strbuf_append_char(code, *p);
                break;
        }
    }
    anvil_strbuf_append(code, "\"\n");
}

static void mir_emit_string_literals(arm64_mir_emit_t *emit)
{
    size_t count = anvil_mir_num_string_literals(emit->mir);
    if (count == 0) return;

    if (emit->is_darwin) {
        anvil_strbuf_append(&emit->code,
                            "\t.section __TEXT,__cstring,cstring_literals\n");
    } else {
        anvil_strbuf_append(&emit->code, "\t.section .rodata\n");
    }

    for (size_t i = 0; i < count; i++) {
        anvil_mir_string_literal_info_t info;
        if (!anvil_mir_get_string_literal_info(emit->mir, i, &info) ||
            !info.label) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "%s:\n", info.label);
        mir_emit_escaped_string(&emit->code, info.value);
    }
}

bool anvil_arm64_emit_mir_abi(const anvil_mir_func_t *mir,
                              anvil_abi_t abi,
                              char **output,
                              size_t *len)
{
    if (!mir || !output) return false;
    *output = NULL;
    if (len) *len = 0;
    if (!anvil_arm64_verify_mir_legal(mir, NULL, 0)) return false;

    arm64_mir_emit_t emit;
    memset(&emit, 0, sizeof(emit));
    emit.mir = mir;
    emit.is_darwin = abi == ANVIL_ABI_DARWIN;
    anvil_strbuf_init(&emit.code);
    if (!emit.code.data) return false;

    if (!mir_prepare_frame(&emit)) {
        anvil_strbuf_destroy(&emit.code);
        free(emit.spill_offsets);
        free(emit.frame_slot_offsets);
        return false;
    }

    mir_emit_prologue(&emit);

    size_t num_blocks = anvil_mir_num_blocks(mir);
    size_t num_instrs = anvil_mir_num_instrs(mir);
    for (size_t b = 0; b < num_blocks && !emit.failed && !emit.code.failed; b++) {
        if (!mir_emit_label(&emit, (anvil_mir_block_t)b)) {
            emit.failed = true;
            break;
        }

        for (size_t i = 0; i < num_instrs && !emit.failed && !emit.code.failed; i++) {
            anvil_mir_instr_info_t info;
            if (!anvil_mir_get_instr_info(mir, i, &info)) {
                emit.failed = true;
                break;
            }
            if (info.block != (anvil_mir_block_t)b) continue;
            mir_emit_instr(&emit, i, &info);
        }
    }

    if (!emit.failed && !emit.code.failed && !emit.is_darwin) {
        const char *name = anvil_mir_func_name(mir);
        anvil_strbuf_appendf(&emit.code, "\t.size %s, .-%s\n", name, name);
    }
    if (!emit.failed && !emit.code.failed) {
        mir_emit_string_literals(&emit);
    }

    free(emit.spill_offsets);
    free(emit.frame_slot_offsets);
    if (emit.failed || emit.code.failed) {
        anvil_strbuf_destroy(&emit.code);
        return false;
    }

    *output = anvil_strbuf_detach(&emit.code, len);
    return *output != NULL;
}

bool anvil_arm64_emit_mir(const anvil_mir_func_t *mir,
                          char **output,
                          size_t *len)
{
    return anvil_arm64_emit_mir_abi(mir, ANVIL_ABI_SYSV, output, len);
}
