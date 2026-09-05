#include "x86_32_internal.h"
#include "anvil/anvil_analysis.h"

#include <limits.h>
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
    anvil_mir_vreg_t hi;
    anvil_mir_vreg_t lo;
    bool is_unsigned;
} value_pair_t;

typedef struct {
    anvil_value_t *value;
    int64_t imm;
    bool valid;
} value_wide_const_t;

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
    const anvil_x86_cc_desc_t *desc;
    anvil_func_t *func;
    anvil_mir_func_t *mir;
    value_vreg_t *values;
    size_t num_values;
    size_t cap_values;
    value_pair_t *pairs;
    size_t num_pairs;
    size_t cap_pairs;
    value_wide_const_t *wide_consts;
    size_t num_wide_consts;
    size_t cap_wide_consts;
    value_addr_offset_t *addr_offsets;
    size_t num_addr_offsets;
    size_t cap_addr_offsets;
    block_map_t *blocks;
    size_t num_blocks;
    size_t num_edge_blocks;
} x86_mir_lower_t;

static bool x86_type_is_void(anvil_type_t *type)
{
    return !type || type->kind == ANVIL_TYPE_VOID;
}

static bool x86_type_is_signed(anvil_type_t *type)
{
    return type && type->is_signed && !x86_type_is_float(type);
}

static anvil_mir_reg_class_t x86_reg_class_for_type(anvil_type_t *type)
{
    return x86_type_is_float(type) ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR;
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

    size_t size = anvil_type_size(type);
    if (size == 0)
        return 32;
    if (size >= 4)
        return 32;
    return (uint16_t)(size * 8);
}

static uint16_t x86_slot_bits_for_type(anvil_type_t *type)
{
    size_t size = type ? x86_type_size(type) : 4;
    if (size == 0)
        size = 4;
    if (size > UINT16_MAX / 8)
        size = UINT16_MAX / 8;
    return (uint16_t)(size * 8);
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
    return anvil_mir_add_vreg_typed(lower->mir, x86_reg_class_for_type(type), x86_bits_for_type(type), x86_type_is_signed(type));
}

static anvil_mir_vreg_t x86_add_i32_vreg(x86_mir_lower_t *lower, bool is_signed)
{
    return anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, is_signed);
}

static anvil_mir_vreg_t x86_add_bool_vreg(x86_mir_lower_t *lower)
{
    return anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
}

static bool map_reserve(x86_mir_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_values)
        return true;

    size_t new_cap = lower->cap_values ? lower->cap_values * 2 : 32;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2)
            return false;
        new_cap *= 2;
    }

    value_vreg_t *grown = realloc(lower->values, new_cap * sizeof(*grown));
    if (!grown)
        return false;

    lower->values = grown;
    lower->cap_values = new_cap;
    return true;
}

static bool map_put(x86_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t vreg)
{
    if (!value || vreg == ANVIL_MIR_NO_VREG)
        return false;

    for (size_t i = 0; i < lower->num_values; i++) {
        if (lower->values[i].value == value) {
            lower->values[i].vreg = vreg;
            return true;
        }
    }

    if (!map_reserve(lower, lower->num_values + 1))
        return false;
    lower->values[lower->num_values].value = value;
    lower->values[lower->num_values].vreg = vreg;
    lower->num_values++;
    return true;
}

static anvil_mir_vreg_t map_get(x86_mir_lower_t *lower, anvil_value_t *value)
{
    for (size_t i = 0; i < lower->num_values; i++) {
        if (lower->values[i].value == value)
            return lower->values[i].vreg;
    }
    return ANVIL_MIR_NO_VREG;
}

static bool pair_reserve(x86_mir_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_pairs)
        return true;
    size_t new_cap = lower->cap_pairs ? lower->cap_pairs * 2 : 16;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2)
            return false;
        new_cap *= 2;
    }
    value_pair_t *grown = realloc(lower->pairs, new_cap * sizeof(*grown));
    if (!grown)
        return false;
    lower->pairs = grown;
    lower->cap_pairs = new_cap;
    return true;
}

static bool pair_put(x86_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t hi, anvil_mir_vreg_t lo, bool is_unsigned)
{
    if (!value || hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG) {
        return false;
    }
    for (size_t i = 0; i < lower->num_pairs; i++) {
        if (lower->pairs[i].value == value) {
            lower->pairs[i].hi = hi;
            lower->pairs[i].lo = lo;
            lower->pairs[i].is_unsigned = is_unsigned;
            return true;
        }
    }
    if (!pair_reserve(lower, lower->num_pairs + 1))
        return false;
    lower->pairs[lower->num_pairs].value = value;
    lower->pairs[lower->num_pairs].hi = hi;
    lower->pairs[lower->num_pairs].lo = lo;
    lower->pairs[lower->num_pairs].is_unsigned = is_unsigned;
    lower->num_pairs++;
    return true;
}

static bool pair_get(x86_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *hi, anvil_mir_vreg_t *lo, bool *is_unsigned)
{
    if (!value)
        return false;
    for (size_t i = 0; i < lower->num_pairs; i++) {
        if (lower->pairs[i].value == value) {
            if (hi)
                *hi = lower->pairs[i].hi;
            if (lo)
                *lo = lower->pairs[i].lo;
            if (is_unsigned)
                *is_unsigned = lower->pairs[i].is_unsigned;
            return true;
        }
    }
    return false;
}

static bool wide_const_reserve(x86_mir_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_wide_consts)
        return true;
    size_t new_cap = lower->cap_wide_consts ? lower->cap_wide_consts * 2 : 16;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2)
            return false;
        new_cap *= 2;
    }
    value_wide_const_t *grown = realloc(lower->wide_consts, new_cap * sizeof(*grown));
    if (!grown)
        return false;
    lower->wide_consts = grown;
    lower->cap_wide_consts = new_cap;
    return true;
}

static bool wide_const_put(x86_mir_lower_t *lower, anvil_value_t *value, int64_t imm)
{
    if (!value)
        return false;
    for (size_t i = 0; i < lower->num_wide_consts; i++) {
        if (lower->wide_consts[i].value == value) {
            lower->wide_consts[i].imm = imm;
            lower->wide_consts[i].valid = true;
            return true;
        }
    }
    if (!wide_const_reserve(lower, lower->num_wide_consts + 1))
        return false;
    lower->wide_consts[lower->num_wide_consts].value = value;
    lower->wide_consts[lower->num_wide_consts].imm = imm;
    lower->wide_consts[lower->num_wide_consts].valid = true;
    lower->num_wide_consts++;
    return true;
}

static bool wide_const_get(x86_mir_lower_t *lower, anvil_value_t *value, int64_t *out_imm)
{
    if (value && value->kind == ANVIL_VAL_CONST_INT) {
        if (out_imm)
            *out_imm = value->data.i;
        return true;
    }
    if (!value)
        return false;
    for (size_t i = 0; i < lower->num_wide_consts; i++) {
        if (lower->wide_consts[i].value == value && lower->wide_consts[i].valid) {
            if (out_imm)
                *out_imm = lower->wide_consts[i].imm;
            return true;
        }
    }
    return false;
}

static bool addr_map_reserve(x86_mir_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_addr_offsets)
        return true;

    size_t new_cap = lower->cap_addr_offsets ? lower->cap_addr_offsets * 2 : 16;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2)
            return false;
        new_cap *= 2;
    }

    value_addr_offset_t *grown = realloc(lower->addr_offsets, new_cap * sizeof(*grown));
    if (!grown)
        return false;

    lower->addr_offsets = grown;
    lower->cap_addr_offsets = new_cap;
    return true;
}

static bool addr_map_put(x86_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t base, int64_t offset)
{
    if (!value || base == ANVIL_MIR_NO_VREG)
        return false;

    for (size_t i = 0; i < lower->num_addr_offsets; i++) {
        if (lower->addr_offsets[i].value == value) {
            lower->addr_offsets[i].base = base;
            lower->addr_offsets[i].offset = offset;
            return true;
        }
    }

    if (!addr_map_reserve(lower, lower->num_addr_offsets + 1))
        return false;
    lower->addr_offsets[lower->num_addr_offsets].value = value;
    lower->addr_offsets[lower->num_addr_offsets].base = base;
    lower->addr_offsets[lower->num_addr_offsets].offset = offset;
    lower->num_addr_offsets++;
    return true;
}

static bool addr_map_get(x86_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *out_base, int64_t *out_offset)
{
    if (!value)
        return false;
    for (size_t i = 0; i < lower->num_addr_offsets; i++) {
        if (lower->addr_offsets[i].value == value) {
            if (out_base)
                *out_base = lower->addr_offsets[i].base;
            if (out_offset)
                *out_offset = lower->addr_offsets[i].offset;
            return true;
        }
    }
    return false;
}

static anvil_mir_block_t block_get(x86_mir_lower_t *lower, anvil_block_t *block)
{
    if (!block)
        return ANVIL_MIR_NO_BLOCK;
    for (size_t i = 0; i < lower->num_blocks; i++) {
        if (lower->blocks[i].block == block)
            return lower->blocks[i].mir_block;
    }
    return ANVIL_MIR_NO_BLOCK;
}

static bool create_mir_blocks(x86_mir_lower_t *lower)
{
    lower->num_blocks = lower->func->num_blocks;
    if (lower->num_blocks == 0)
        return true;

    lower->blocks = calloc(lower->num_blocks, sizeof(*lower->blocks));
    if (!lower->blocks)
        return false;

    size_t index = 0;
    for (anvil_block_t *block = lower->func->blocks; block; block = block->next) {
        anvil_mir_block_t mir_block;
        if (index == 0) {
            mir_block = anvil_mir_current_block(lower->mir);
        } else {
            mir_block = anvil_mir_add_block(lower->mir, block->name);
        }
        if (mir_block == ANVIL_MIR_NO_BLOCK)
            return false;

        lower->blocks[index].block = block;
        lower->blocks[index].mir_block = mir_block;
        index++;
    }

    return true;
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

    anvil_mir_block_t entry = block_get(lower, lower->func->blocks);
    if (entry == ANVIL_MIR_NO_BLOCK || !anvil_mir_set_current_block(lower->mir, entry)) {
        return false;
    }

    for (size_t i = 0; i < lower->func->num_params; i++) {
        anvil_value_t *param = lower->func->params[i];
        if (!param)
            return false;

        if (x86_needs_pair(param->type)) {
            bool is_unsigned = param->type->kind == ANVIL_TYPE_U64;
            anvil_mir_vreg_t lo = x86_add_i32_vreg(lower, false);
            anvil_mir_vreg_t hi = x86_add_i32_vreg(lower, !is_unsigned);
            if (lo == ANVIL_MIR_NO_VREG || hi == ANVIL_MIR_NO_VREG)
                return false;
            if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, lo, stack_offset)) {
                return false;
            }
            if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, hi, stack_offset + 4)) {
                return false;
            }
            stack_offset += 8;
            if (!pair_put(lower, param, hi, lo, is_unsigned))
                return false;
            continue;
        }

        anvil_mir_vreg_t local = x86_add_vreg_for_type(lower, param->type);
        if (local == ANVIL_MIR_NO_VREG)
            return false;

        if (x86_arg_uses_int_reg(desc, param->type, int_reg_count)) {
            int reg = desc->reg_int_args[int_reg_count];
            anvil_mir_vreg_t incoming = x86_add_vreg_for_type(lower, param->type);
            if (incoming == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->mir, incoming, reg) || !anvil_mir_set_live_in(lower->mir, incoming, true)) {
                return false;
            }
            anvil_mir_vreg_t uses[] = {incoming};
            if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, local, uses, 1)) {
                return false;
            }
            int_reg_count++;
        } else {
            if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, local, stack_offset)) {
                return false;
            }
            stack_offset += x86_stack_arg_slot_size(param->type);
        }

        if (!map_put(lower, param, local))
            return false;
    }

    return true;
}

static bool prepare_phi_results(x86_mir_lower_t *lower)
{
    for (anvil_block_t *block = lower->func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_PHI)
                break;
            if (!instr->result)
                return false;
            if (x86_needs_pair(instr->result->type))
                return false;

            anvil_mir_vreg_t vreg = x86_add_vreg_for_type(lower, instr->result->type);
            if (vreg == ANVIL_MIR_NO_VREG)
                return false;
            if (!map_put(lower, instr->result, vreg))
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

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, vreg, imm)) {
        return ANVIL_MIR_NO_VREG;
    }
    return vreg;
}

static bool lower_i64_const_pair(x86_mir_lower_t *lower, anvil_value_t *value)
{
    anvil_mir_vreg_t exist_hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t exist_lo = ANVIL_MIR_NO_VREG;
    if (pair_get(lower, value, &exist_hi, &exist_lo, NULL))
        return true;
    if (value->kind != ANVIL_VAL_CONST_INT)
        return false;

    int64_t imm = value->data.i;
    uint64_t bits = (uint64_t)imm;
    int32_t hi_imm = (int32_t)(bits >> 32);
    int32_t lo_imm = (int32_t)(bits & 0xffffffffu);
    bool is_unsigned = value->type && value->type->kind == ANVIL_TYPE_U64;

    anvil_mir_vreg_t lo = x86_add_i32_vreg(lower, false);
    anvil_mir_vreg_t hi = x86_add_i32_vreg(lower, !is_unsigned);
    if (lo == ANVIL_MIR_NO_VREG || hi == ANVIL_MIR_NO_VREG)
        return false;

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, lo, (int64_t)lo_imm) || !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, hi, (int64_t)hi_imm)) {
        return false;
    }

    return wide_const_put(lower, value, imm) && pair_put(lower, value, hi, lo, is_unsigned);
}

static bool ensure_i64_pair(x86_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *hi, anvil_mir_vreg_t *lo, bool *is_unsigned)
{
    if (pair_get(lower, value, hi, lo, is_unsigned))
        return true;
    if (value && value->kind == ANVIL_VAL_CONST_INT && lower_i64_const_pair(lower, value)) {
        return pair_get(lower, value, hi, lo, is_unsigned);
    }
    return false;
}

static anvil_mir_vreg_t lower_symbol_address(x86_mir_lower_t *lower, const char *symbol)
{
    if (!symbol || !symbol[0])
        return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t vreg = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, false);
    if (vreg == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;

    if (!anvil_mir_add_instr_symbol(lower->mir, ANVIL_MIR_OP_SYMBOL_ADDR, vreg, NULL, 0, symbol)) {
        return ANVIL_MIR_NO_VREG;
    }
    return vreg;
}

static anvil_mir_vreg_t lower_reloc_address(x86_mir_lower_t *lower, anvil_value_t *value)
{
    const char *symbol = value && value->data.reloc.symbol ? value->data.reloc.symbol->name : NULL;
    if (!symbol || !symbol[0])
        return ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t vreg = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, false);
    if (vreg == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_symbol_imm(lower->mir, ANVIL_MIR_OP_SYMBOL_ADDR, vreg, NULL, 0, symbol, value->data.reloc.addend))
        return ANVIL_MIR_NO_VREG;
    return vreg;
}

static anvil_mir_vreg_t lower_string_address(x86_mir_lower_t *lower, anvil_value_t *value)
{
    const char *label = NULL;
    if (anvil_mir_add_string_literal(lower->mir, value->data.str, &label) < 0 || !label) {
        return ANVIL_MIR_NO_VREG;
    }
    return lower_symbol_address(lower, label);
}

static anvil_mir_vreg_t lower_value(x86_mir_lower_t *lower, anvil_value_t *value)
{
    if (!value)
        return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t existing = map_get(lower, value);
    if (existing != ANVIL_MIR_NO_VREG)
        return existing;

    anvil_mir_vreg_t base = ANVIL_MIR_NO_VREG;
    int64_t offset = 0;
    if (addr_map_get(lower, value, &base, &offset)) {
        anvil_mir_vreg_t materialized = ANVIL_MIR_NO_VREG;
        if (!lower_add_const_offset(lower, base, offset, &materialized)) {
            return ANVIL_MIR_NO_VREG;
        }
        if (!map_put(lower, value, materialized))
            return ANVIL_MIR_NO_VREG;
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
    if (!callee)
        return NULL;
    if (callee->kind == ANVIL_VAL_FUNC && callee->data.func) {
        return callee->data.func->name;
    }
    if (callee->kind == ANVIL_VAL_GLOBAL && callee->type && callee->type->kind == ANVIL_TYPE_FUNC && callee->name) {
        return callee->name;
    }
    return NULL;
}

static bool call_is_direct_symbol(anvil_value_t *callee)
{
    if (!callee)
        return false;
    if (callee->kind == ANVIL_VAL_FUNC)
        return true;
    return callee->kind == ANVIL_VAL_GLOBAL && callee->type && callee->type->kind == ANVIL_TYPE_FUNC;
}

static anvil_type_t *call_func_type(anvil_value_t *callee)
{
    if (!callee || !callee->type)
        return NULL;
    if (callee->type->kind == ANVIL_TYPE_FUNC)
        return callee->type;
    if (callee->type->kind == ANVIL_TYPE_PTR && callee->type->data.pointee && callee->type->data.pointee->kind == ANVIL_TYPE_FUNC) {
        return callee->type->data.pointee;
    }
    return NULL;
}

static bool add_return(x86_mir_lower_t *lower, anvil_value_t *value)
{
    if (!value) {
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, NULL, 0);
    }

    if (x86_needs_pair(value->type)) {
        anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
        anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
        if (!ensure_i64_pair(lower, value, &hi, &lo, NULL))
            return false;

        anvil_mir_vreg_t hi_use[] = {hi};
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET_VALUE_PART, ANVIL_MIR_NO_VREG, hi_use, 1)) {
            return false;
        }
        anvil_mir_vreg_t uses[] = {lo};
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, uses, 1);
    }

    anvil_mir_vreg_t src = lower_value(lower, value);
    if (src == ANVIL_MIR_NO_VREG)
        return false;

    anvil_mir_vreg_t ret_uses[] = {src};
    return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, ret_uses, 1);
}

static bool block_starts_with_phi(anvil_block_t *block)
{
    return block && block->first && block->first->op == ANVIL_OP_PHI;
}

static bool append_phi_edge_copy(phi_edge_copy_t **copies, size_t *num_copies, size_t *cap_copies, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    if (dst == ANVIL_MIR_NO_VREG || src == ANVIL_MIR_NO_VREG)
        return false;
    if (dst == src)
        return true;

    if (*num_copies >= *cap_copies) {
        size_t new_cap = *cap_copies ? *cap_copies * 2 : 4;
        if (new_cap < *num_copies + 1)
            return false;

        phi_edge_copy_t *grown = realloc(*copies, new_cap * sizeof(*grown));
        if (!grown)
            return false;

        *copies = grown;
        *cap_copies = new_cap;
    }

    (*copies)[*num_copies].dst = dst;
    (*copies)[*num_copies].src = src;
    (*num_copies)++;
    return true;
}

static bool emit_vreg_copy(anvil_mir_func_t *mir, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    anvil_mir_vreg_t uses[] = {src};
    return anvil_mir_add_instr(mir, ANVIL_MIR_OP_COPY, dst, uses, 1);
}

static bool lower_phi_copies_for_edge(x86_mir_lower_t *lower, anvil_block_t *src_block, anvil_block_t *dest_block)
{
    if (!block_starts_with_phi(dest_block))
        return true;

    phi_edge_copy_t *copies = NULL;
    size_t num_copies = 0;
    size_t cap_copies = 0;

    for (anvil_instr_t *phi = dest_block->first; phi; phi = phi->next) {
        if (phi->op != ANVIL_OP_PHI)
            break;
        if (!phi->result)
            goto fail;

        anvil_mir_vreg_t phi_vreg = map_get(lower, phi->result);
        if (phi_vreg == ANVIL_MIR_NO_VREG)
            goto fail;

        bool found = false;
        for (size_t i = 0; i < phi->num_phi_incoming; i++) {
            if (phi->phi_blocks && phi->phi_blocks[i] == src_block) {
                if (i >= phi->num_operands || !phi->operands[i])
                    goto fail;

                anvil_mir_vreg_t incoming = lower_value(lower, phi->operands[i]);
                if (incoming == ANVIL_MIR_NO_VREG)
                    goto fail;
                if (!append_phi_edge_copy(&copies, &num_copies, &cap_copies, phi_vreg, incoming)) {
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

static anvil_mir_block_t create_phi_edge_block(x86_mir_lower_t *lower, anvil_block_t *src_block, anvil_block_t *dest_block)
{
    const char *src_name = (src_block && src_block->name) ? src_block->name : "anon";
    const char *dest_name = (dest_block && dest_block->name) ? dest_block->name : "anon";

    char name[256];
    snprintf(name, sizeof(name), "%s_to_%s_phi_%zu", src_name, dest_name, lower->num_edge_blocks++);
    anvil_mir_block_t source = anvil_mir_current_block(lower->mir);
    anvil_mir_block_t edge = anvil_mir_add_block(lower->mir, name);
    if (!anvil_mir_set_current_block(lower->mir, source))
        return ANVIL_MIR_NO_BLOCK;

    return edge;
}

static bool emit_phi_edge_block(x86_mir_lower_t *lower, anvil_block_t *src_block, anvil_block_t *dest_block, anvil_mir_block_t edge_block, anvil_mir_block_t dest_mir_block)
{
    if (!anvil_mir_set_current_block(lower->mir, edge_block))
        return false;
    if (!lower_phi_copies_for_edge(lower, src_block, dest_block))
        return false;
    return anvil_mir_add_branch(lower->mir, dest_mir_block);
}

static bool append_pending_phi_edge(pending_phi_edge_t **edges, size_t *num_edges, size_t *cap_edges, anvil_block_t *dest_block, anvil_mir_block_t edge_block, anvil_mir_block_t dest_mir_block)
{
    if (!dest_block || edge_block == ANVIL_MIR_NO_BLOCK || dest_mir_block == ANVIL_MIR_NO_BLOCK) {
        return false;
    }

    if (*num_edges >= *cap_edges) {
        size_t new_cap = *cap_edges ? *cap_edges * 2 : 4;
        if (new_cap < *num_edges + 1)
            return false;

        pending_phi_edge_t *grown = realloc(*edges, new_cap * sizeof(*grown));
        if (!grown)
            return false;

        *edges = grown;
        *cap_edges = new_cap;
    }

    (*edges)[*num_edges].dest_block = dest_block;
    (*edges)[*num_edges].edge_block = edge_block;
    (*edges)[*num_edges].dest_mir_block = dest_mir_block;
    (*num_edges)++;
    return true;
}

static bool prepare_phi_aware_target(x86_mir_lower_t *lower, anvil_block_t *src_block, anvil_block_t *dest_block, anvil_mir_block_t *out_target, pending_phi_edge_t **edges, size_t *num_edges,
                                     size_t *cap_edges)
{
    anvil_mir_block_t dest_mir = block_get(lower, dest_block);
    if (dest_mir == ANVIL_MIR_NO_BLOCK)
        return false;

    *out_target = dest_mir;
    if (!block_starts_with_phi(dest_block))
        return true;

    anvil_mir_block_t edge_block = create_phi_edge_block(lower, src_block, dest_block);
    if (edge_block == ANVIL_MIR_NO_BLOCK)
        return false;
    *out_target = edge_block;
    return append_pending_phi_edge(edges, num_edges, cap_edges, dest_block, edge_block, dest_mir);
}

static bool emit_pending_phi_edges(x86_mir_lower_t *lower, anvil_block_t *src_block, pending_phi_edge_t *edges, size_t num_edges)
{
    for (size_t i = 0; i < num_edges; i++) {
        if (!emit_phi_edge_block(lower, src_block, edges[i].dest_block, edges[i].edge_block, edges[i].dest_mir_block)) {
            return false;
        }
    }
    return true;
}

static anvil_mir_block_t create_switch_chain_block(x86_mir_lower_t *lower, anvil_block_t *src_block, size_t case_index)
{
    const char *src_name = (src_block && src_block->name) ? src_block->name : "anon";

    char name[256];
    snprintf(name, sizeof(name), "%s_switch_case_%zu_%zu", src_name, case_index, lower->num_edge_blocks++);
    return anvil_mir_add_block(lower->mir, name);
}

static bool lower_call(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    const anvil_x86_cc_desc_t *desc = anvil_x86_get_cc_desc(instr->call_cc);
    if (!desc)
        return false;
    if (instr->num_operands == 0)
        return false;

    anvil_value_t *callee = instr->operands[0];
    anvil_type_t *fn_type = call_func_type(callee);
    if (!fn_type)
        return false;

    bool direct_call = call_is_direct_symbol(callee);
    const char *symbol = direct_call ? call_symbol(callee) : NULL;
    if (direct_call && !symbol)
        return false;

    size_t num_args = instr->num_operands - 1;
    size_t max_call_uses = num_args + (direct_call ? 0 : 1) + 1;
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
            if (!anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, lo_use, 1, stack_offset) ||
                !anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, hi_use, 1, stack_offset + 4)) {
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

        if (x86_arg_uses_int_reg(desc, arg->type, int_reg_count)) {
            call_uses[num_call_uses++] = src;
            int_reg_count++;
            continue;
        }

        anvil_mir_vreg_t stack_use[] = {src};
        if (!anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, stack_use, 1, stack_offset)) {
            ok = false;
            break;
        }
        stack_offset += x86_stack_arg_slot_size(arg->type);
    }

    if (!ok) {
        free(call_uses);
        return false;
    }

    bool result_is_pair = instr->result && x86_needs_pair(instr->result->type);
    bool result_is_fp = instr->result && x86_type_is_float(instr->result->type);
    bool result_is_int = instr->result && !x86_type_is_void(instr->result->type) && !result_is_pair && !result_is_fp;

    anvil_mir_vreg_t call_def = ANVIL_MIR_NO_VREG;
    if (result_is_int) {
        call_def = x86_add_vreg_for_type(lower, instr->result->type);
        if (call_def == ANVIL_MIR_NO_VREG) {
            free(call_uses);
            return false;
        }
    } else if (result_is_pair) {
        call_def = x86_add_i32_vreg(lower, false);
        if (call_def == ANVIL_MIR_NO_VREG) {
            free(call_uses);
            return false;
        }
    }

    bool emit_ok = anvil_mir_add_call(lower->mir, call_def, call_uses, num_call_uses, symbol, instr->call_cc, false, 0);
    free(call_uses);
    if (!emit_ok)
        return false;

    if (!instr->result || x86_type_is_void(instr->result->type)) {
        return true;
    }

    if (result_is_pair) {
        bool is_unsigned = instr->result->type->kind == ANVIL_TYPE_U64;
        anvil_mir_vreg_t hi = x86_add_i32_vreg(lower, !is_unsigned);
        if (hi == ANVIL_MIR_NO_VREG)
            return false;
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_CALL_RESULT, hi, NULL, 0)) {
            return false;
        }
        return pair_put(lower, instr->result, hi, call_def, is_unsigned);
    }

    if (result_is_fp) {
        anvil_mir_vreg_t local_result = x86_add_vreg_for_type(lower, instr->result->type);
        if (local_result == ANVIL_MIR_NO_VREG)
            return false;
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_CALL_RESULT, local_result, NULL, 0)) {
            return false;
        }
        return map_put(lower, instr->result, local_result);
    }

    return map_put(lower, instr->result, call_def);
}

static anvil_mir_vreg_t lower_widen_gpr_to_32(x86_mir_lower_t *lower, anvil_mir_vreg_t src, bool sign_extend)
{
    const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->mir, src);
    if (!src_info || src_info->reg_class != ANVIL_MIR_REG_GPR) {
        return ANVIL_MIR_NO_VREG;
    }
    if (src_info->size_bits == 32)
        return src;

    if (src_info->size_bits > 32) {
        anvil_mir_vreg_t narrow = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, sign_extend);
        if (narrow == ANVIL_MIR_NO_VREG)
            return ANVIL_MIR_NO_VREG;
        anvil_mir_vreg_t uses[] = {src};
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_TRUNC, narrow, uses, 1)) {
            return ANVIL_MIR_NO_VREG;
        }
        return narrow;
    }

    anvil_mir_vreg_t wide = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, sign_extend);
    if (wide == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t uses[] = {src};
    anvil_mir_opcode_t op = sign_extend ? ANVIL_MIR_OP_SEXT : ANVIL_MIR_OP_ZEXT;
    if (!anvil_mir_add_instr(lower->mir, op, wide, uses, 1)) {
        return ANVIL_MIR_NO_VREG;
    }
    return wide;
}

static anvil_mir_vreg_t lower_resize_gpr(x86_mir_lower_t *lower, anvil_mir_vreg_t src, uint16_t target_bits)
{
    const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->mir, src);
    if (!src_info || src_info->reg_class != ANVIL_MIR_REG_GPR || src_info->size_bits == 0 || target_bits == 0) {
        return ANVIL_MIR_NO_VREG;
    }
    if (src_info->size_bits == target_bits)
        return src;

    /* Capture src fields before anvil_mir_add_vreg_typed: it may realloc the
     * vreg array, invalidating src_info. */
    uint16_t src_bits = src_info->size_bits;
    bool src_signed = src_info->is_signed;

    anvil_mir_vreg_t resized = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, target_bits, src_signed);
    if (resized == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t uses[] = {src};
    anvil_mir_opcode_t op = ANVIL_MIR_OP_TRUNC;
    if (src_bits < target_bits) {
        op = src_signed ? ANVIL_MIR_OP_SEXT : ANVIL_MIR_OP_ZEXT;
    }
    if (!anvil_mir_add_instr(lower->mir, op, resized, uses, 1)) {
        return ANVIL_MIR_NO_VREG;
    }
    return resized;
}

static bool lower_match_binary_operand_sizes(x86_mir_lower_t *lower, anvil_mir_vreg_t *lhs, anvil_mir_vreg_t *rhs, anvil_mir_vreg_t def)
{
    const anvil_mir_vreg_info_t *def_info = anvil_mir_get_vreg_info(lower->mir, def);
    const anvil_mir_vreg_info_t *lhs_info = anvil_mir_get_vreg_info(lower->mir, *lhs);
    const anvil_mir_vreg_info_t *rhs_info = anvil_mir_get_vreg_info(lower->mir, *rhs);
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
        int slot = anvil_mir_add_frame_slot(lower->mir, x86_slot_bits_for_type(element_type), x86_align_for_type(element_type));
        if (slot < 0)
            return false;
        if (!anvil_mir_add_frame_addr(lower->mir, ptr, slot))
            return false;
        return map_put(lower, instr->result, ptr);
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
    if (!anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_DYN_ALLOCA, ptr, uses, 1, elem_size)) {
        return false;
    }
    return map_put(lower, instr->result, ptr);
}

static bool lower_add_const_offset(x86_mir_lower_t *lower, anvil_mir_vreg_t base, int64_t offset, anvil_mir_vreg_t *out_ptr)
{
    if (offset == 0) {
        *out_ptr = base;
        return true;
    }

    anvil_mir_vreg_t off = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t dst = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 32);
    if (off == ANVIL_MIR_NO_VREG || dst == ANVIL_MIR_NO_VREG)
        return false;
    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, off, offset)) {
        return false;
    }
    anvil_mir_vreg_t uses[] = {base, off};
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_ADD, dst, uses, 2)) {
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
            anvil_mir_vreg_t scale = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 32);
            scaled = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 32);
            if (scale == ANVIL_MIR_NO_VREG || scaled == ANVIL_MIR_NO_VREG) {
                return false;
            }
            if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, scale, elem_size)) {
                return false;
            }
            anvil_mir_vreg_t mul_uses[] = {index, scale};
            if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_MUL, scaled, mul_uses, 2)) {
                return false;
            }
        }

        anvil_mir_vreg_t next = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 32);
        if (next == ANVIL_MIR_NO_VREG)
            return false;
        anvil_mir_vreg_t add_uses[] = {current, scaled};
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_ADD, next, add_uses, 2)) {
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

    return addr_map_put(lower, instr->result, base, offset);
}

static bool lower_cast(x86_mir_lower_t *lower, anvil_instr_t *instr, anvil_mir_opcode_t mir_op)
{
    if (instr->num_operands != 1 || !instr->result)
        return false;

    bool dst_pair = x86_needs_pair(instr->result->type);
    bool src_pair = x86_needs_pair(instr->operands[0]->type);

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
        if (wide_const_get(lower, instr->operands[0], &imm) && !wide_const_put(lower, instr->result, imm)) {
            return false;
        }
        return pair_put(lower, instr->result, hi, lo, is_unsigned);
    }

    if (src_pair)
        return false;

    anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t def = x86_add_vreg_for_type(lower, instr->result->type);
    if (src == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG)
        return false;

    if (instr->result->type->kind == ANVIL_TYPE_I1 && instr->operands[0]->type->kind != ANVIL_TYPE_I1) {
        const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->mir, src);
        if (src_info && src_info->reg_class == ANVIL_MIR_REG_FPR) {
            if (mir_op != ANVIL_MIR_OP_FPTOUI)
                return false;
            anvil_mir_vreg_t converted = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, false);
            anvil_mir_vreg_t convert_use[] = {src};
            if (converted == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(lower->mir, mir_op, converted, convert_use, 1))
                return false;
            src = converted;
            src_info = anvil_mir_get_vreg_info(lower->mir, src);
        }
        anvil_mir_vreg_t narrowed = src;
        if (!src_info || src_info->reg_class != ANVIL_MIR_REG_GPR)
            return false;
        if (src_info->size_bits > 8) {
            narrowed = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
            anvil_mir_vreg_t narrow_use[] = {src};
            if (narrowed == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_TRUNC, narrowed, narrow_use, 1))
                return false;
        }
        anvil_mir_vreg_t one = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
        anvil_mir_vreg_t uses[] = {narrowed, one};
        if (one == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, one, 1) || !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_AND, def, uses, 2))
            return false;
        return map_put(lower, instr->result, def);
    }

    anvil_mir_vreg_t uses[] = {src};
    if (!anvil_mir_add_instr(lower->mir, mir_op, def, uses, 1))
        return false;
    return map_put(lower, instr->result, def);
}

static bool lower_select(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands != 3 || !instr->result)
        return false;
    if (x86_needs_pair(instr->result->type))
        return false;

    anvil_mir_vreg_t cond = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t then_v = lower_value(lower, instr->operands[1]);
    anvil_mir_vreg_t else_v = lower_value(lower, instr->operands[2]);
    anvil_mir_vreg_t def = x86_add_vreg_for_type(lower, instr->result->type);
    if (cond == ANVIL_MIR_NO_VREG || then_v == ANVIL_MIR_NO_VREG || else_v == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) {
        return false;
    }

    anvil_mir_block_t source = anvil_mir_current_block(lower->mir);
    anvil_mir_block_t then_block = create_switch_chain_block(lower, instr->parent, 0);
    anvil_mir_block_t else_block = create_switch_chain_block(lower, instr->parent, 1);
    anvil_mir_block_t join_block = create_switch_chain_block(lower, instr->parent, 2);
    if (then_block == ANVIL_MIR_NO_BLOCK || else_block == ANVIL_MIR_NO_BLOCK || join_block == ANVIL_MIR_NO_BLOCK) {
        return false;
    }

    if (!anvil_mir_set_current_block(lower->mir, source) || !anvil_mir_add_cond_branch(lower->mir, cond, then_block, else_block)) {
        return false;
    }

    if (!anvil_mir_set_current_block(lower->mir, then_block) || !emit_vreg_copy(lower->mir, def, then_v) || !anvil_mir_add_branch(lower->mir, join_block)) {
        return false;
    }

    if (!anvil_mir_set_current_block(lower->mir, else_block) || !emit_vreg_copy(lower->mir, def, else_v) || !anvil_mir_add_branch(lower->mir, join_block)) {
        return false;
    }

    if (!anvil_mir_set_current_block(lower->mir, join_block))
        return false;
    return map_put(lower, instr->result, def);
}

static bool lower_memory_address(x86_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *out_base, int64_t *out_offset)
{
    anvil_mir_vreg_t base = ANVIL_MIR_NO_VREG;
    int64_t offset = 0;
    if (addr_map_get(lower, value, &base, &offset)) {
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
    bool ok = offset == 0 ? anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_LOAD, lo, lo_uses, 1) : anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_LOAD, lo, lo_uses, 1, offset);
    if (!ok)
        return false;

    anvil_mir_vreg_t hi_uses[] = {base};
    if (!anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_LOAD, hi, hi_uses, 1, offset + 4)) {
        return false;
    }

    return pair_put(lower, instr->result, hi, lo, is_unsigned);
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
    bool ok = offset == 0 ? anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, lo_uses, 2)
                          : anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, lo_uses, 2, offset);
    if (!ok)
        return false;

    anvil_mir_vreg_t hi_uses[] = {hi, base};
    return anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, hi_uses, 2, offset + 4);
}

static bool add_pair_cmp(x86_mir_lower_t *lower, anvil_mir_opcode_t op, anvil_mir_vreg_t def, anvil_mir_vreg_t a, anvil_mir_vreg_t b)
{
    anvil_mir_vreg_t uses[] = {a, b};
    return anvil_mir_add_instr(lower->mir, op, def, uses, 2);
}

static bool lower_i64_cmp_pair(x86_mir_lower_t *lower, anvil_instr_t *instr, anvil_mir_opcode_t op)
{
    if (!instr->result || instr->num_operands != 2)
        return false;

    anvil_mir_vreg_t lhi = ANVIL_MIR_NO_VREG, llo = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t rhi = ANVIL_MIR_NO_VREG, rlo = ANVIL_MIR_NO_VREG;
    if (!ensure_i64_pair(lower, instr->operands[0], &lhi, &llo, NULL) || !ensure_i64_pair(lower, instr->operands[1], &rhi, &rlo, NULL)) {
        return false;
    }

    anvil_mir_vreg_t result = x86_add_bool_vreg(lower);
    if (result == ANVIL_MIR_NO_VREG)
        return false;

    if (op == ANVIL_MIR_OP_CMP_EQ || op == ANVIL_MIR_OP_CMP_NE) {
        anvil_mir_vreg_t hi_cmp = x86_add_bool_vreg(lower);
        anvil_mir_vreg_t lo_cmp = x86_add_bool_vreg(lower);
        if (hi_cmp == ANVIL_MIR_NO_VREG || lo_cmp == ANVIL_MIR_NO_VREG) {
            return false;
        }
        if (!add_pair_cmp(lower, op, hi_cmp, lhi, rhi) || !add_pair_cmp(lower, op, lo_cmp, llo, rlo)) {
            return false;
        }
        anvil_mir_vreg_t uses[] = {hi_cmp, lo_cmp};
        anvil_mir_opcode_t join = op == ANVIL_MIR_OP_CMP_EQ ? ANVIL_MIR_OP_AND : ANVIL_MIR_OP_OR;
        if (!anvil_mir_add_instr(lower->mir, join, result, uses, 2)) {
            return false;
        }
        return map_put(lower, instr->result, result);
    }

    bool unsigned_cmp = op == ANVIL_MIR_OP_CMP_ULT || op == ANVIL_MIR_OP_CMP_ULE || op == ANVIL_MIR_OP_CMP_UGT || op == ANVIL_MIR_OP_CMP_UGE;
    bool less = op == ANVIL_MIR_OP_CMP_LT || op == ANVIL_MIR_OP_CMP_LE || op == ANVIL_MIR_OP_CMP_ULT || op == ANVIL_MIR_OP_CMP_ULE;
    bool equal_ok = op == ANVIL_MIR_OP_CMP_LE || op == ANVIL_MIR_OP_CMP_GE || op == ANVIL_MIR_OP_CMP_ULE || op == ANVIL_MIR_OP_CMP_UGE;

    anvil_mir_opcode_t hi_rel;
    anvil_mir_opcode_t lo_rel;
    if (less) {
        hi_rel = unsigned_cmp ? ANVIL_MIR_OP_CMP_ULT : ANVIL_MIR_OP_CMP_LT;
        lo_rel = equal_ok ? ANVIL_MIR_OP_CMP_ULE : ANVIL_MIR_OP_CMP_ULT;
    } else {
        hi_rel = unsigned_cmp ? ANVIL_MIR_OP_CMP_UGT : ANVIL_MIR_OP_CMP_GT;
        lo_rel = equal_ok ? ANVIL_MIR_OP_CMP_UGE : ANVIL_MIR_OP_CMP_UGT;
    }

    anvil_mir_vreg_t hi_cmp = x86_add_bool_vreg(lower);
    anvil_mir_vreg_t hi_eq = x86_add_bool_vreg(lower);
    anvil_mir_vreg_t lo_cmp = x86_add_bool_vreg(lower);
    anvil_mir_vreg_t eq_and_lo = x86_add_bool_vreg(lower);
    if (hi_cmp == ANVIL_MIR_NO_VREG || hi_eq == ANVIL_MIR_NO_VREG || lo_cmp == ANVIL_MIR_NO_VREG || eq_and_lo == ANVIL_MIR_NO_VREG) {
        return false;
    }

    if (!add_pair_cmp(lower, hi_rel, hi_cmp, lhi, rhi) || !add_pair_cmp(lower, ANVIL_MIR_OP_CMP_EQ, hi_eq, lhi, rhi) || !add_pair_cmp(lower, lo_rel, lo_cmp, llo, rlo)) {
        return false;
    }

    anvil_mir_vreg_t and_uses[] = {hi_eq, lo_cmp};
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_AND, eq_and_lo, and_uses, 2)) {
        return false;
    }
    anvil_mir_vreg_t or_uses[] = {hi_cmp, eq_and_lo};
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_OR, result, or_uses, 2)) {
        return false;
    }
    return map_put(lower, instr->result, result);
}

static bool lower_i64_bitwise_pair(x86_mir_lower_t *lower, anvil_instr_t *instr, anvil_mir_opcode_t op)
{
    if (!instr->result || instr->num_operands != 2)
        return false;

    anvil_mir_vreg_t lhi = ANVIL_MIR_NO_VREG, llo = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t rhi = ANVIL_MIR_NO_VREG, rlo = ANVIL_MIR_NO_VREG;
    bool lu = false;
    if (!ensure_i64_pair(lower, instr->operands[0], &lhi, &llo, &lu) || !ensure_i64_pair(lower, instr->operands[1], &rhi, &rlo, NULL)) {
        return false;
    }

    bool is_unsigned = instr->result->type->kind == ANVIL_TYPE_U64;
    anvil_mir_vreg_t lo = x86_add_i32_vreg(lower, false);
    anvil_mir_vreg_t hi = x86_add_i32_vreg(lower, !is_unsigned);
    if (lo == ANVIL_MIR_NO_VREG || hi == ANVIL_MIR_NO_VREG)
        return false;

    anvil_mir_vreg_t lo_uses[] = {llo, rlo};
    anvil_mir_vreg_t hi_uses[] = {lhi, rhi};
    if (!anvil_mir_add_instr(lower->mir, op, lo, lo_uses, 2) || !anvil_mir_add_instr(lower->mir, op, hi, hi_uses, 2)) {
        return false;
    }

    return pair_put(lower, instr->result, hi, lo, is_unsigned);
}

static bool lower_i64_unop_const(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    int64_t imm = 0;
    if (!wide_const_get(lower, instr->operands[0], &imm))
        return false;

    int64_t result;
    if (instr->op == ANVIL_OP_NEG) {
        result = (int64_t)(0 - (uint64_t)imm);
    } else if (instr->op == ANVIL_OP_NOT) {
        result = ~imm;
    } else {
        return false;
    }

    bool is_unsigned = instr->result->type->kind == ANVIL_TYPE_U64;
    uint64_t bits = (uint64_t)result;
    int32_t hi_imm = (int32_t)(bits >> 32);
    int32_t lo_imm = (int32_t)(bits & 0xffffffffu);

    anvil_mir_vreg_t lo = x86_add_i32_vreg(lower, false);
    anvil_mir_vreg_t hi = x86_add_i32_vreg(lower, !is_unsigned);
    if (lo == ANVIL_MIR_NO_VREG || hi == ANVIL_MIR_NO_VREG)
        return false;
    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, lo, (int64_t)lo_imm) || !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, hi, (int64_t)hi_imm)) {
        return false;
    }
    return wide_const_put(lower, instr->result, result) && pair_put(lower, instr->result, hi, lo, is_unsigned);
}

static bool lower_switch(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands != instr->num_switch_cases + 1 || instr->num_operands < 1 || !instr->true_block) {
        return false;
    }

    anvil_mir_vreg_t selector = lower_value(lower, instr->operands[0]);
    if (selector == ANVIL_MIR_NO_VREG)
        return false;

    pending_phi_edge_t *edges = NULL;
    size_t num_edges = 0;
    size_t cap_edges = 0;

    anvil_mir_block_t default_target = ANVIL_MIR_NO_BLOCK;
    if (!prepare_phi_aware_target(lower, instr->parent, instr->true_block, &default_target, &edges, &num_edges, &cap_edges)) {
        free(edges);
        return false;
    }

    if (instr->num_switch_cases == 0) {
        bool ok = anvil_mir_add_branch(lower->mir, default_target) && emit_pending_phi_edges(lower, instr->parent, edges, num_edges);
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
        if (!prepare_phi_aware_target(lower, instr->parent, instr->switch_blocks[i], &case_target, &edges, &num_edges, &cap_edges)) {
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
        anvil_mir_vreg_t cmp = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
        if (case_value == ANVIL_MIR_NO_VREG || cmp == ANVIL_MIR_NO_VREG) {
            ok = false;
            break;
        }

        anvil_mir_vreg_t cmp_uses[] = {selector, case_value};
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_CMP_EQ, cmp, cmp_uses, 2) || !anvil_mir_add_cond_branch(lower->mir, cmp, case_target, false_target)) {
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

static bool lower_instr(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->op == ANVIL_OP_NOP)
        return true;
    if (instr->op == ANVIL_OP_PHI)
        return true;

    anvil_mir_opcode_t mir_op;
    if (instr->num_operands == 2 && map_binop(instr->op, &mir_op)) {
        bool wide = (instr->operands[0] && x86_needs_pair(instr->operands[0]->type)) || (instr->operands[1] && x86_needs_pair(instr->operands[1]->type)) ||
                    (instr->result && x86_needs_pair(instr->result->type));
        if (wide) {
            if (mir_op_is_compare(mir_op)) {
                return lower_i64_cmp_pair(lower, instr, mir_op);
            }
            if (mir_op == ANVIL_MIR_OP_AND || mir_op == ANVIL_MIR_OP_OR || mir_op == ANVIL_MIR_OP_XOR) {
                return lower_i64_bitwise_pair(lower, instr, mir_op);
            }
            return false;
        }

        anvil_mir_vreg_t lhs = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t rhs = lower_value(lower, instr->operands[1]);
        anvil_mir_vreg_t def = instr->result ? x86_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
        if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) {
            return false;
        }
        if (!mir_op_is_compare(mir_op) && !lower_match_binary_operand_sizes(lower, &lhs, &rhs, def)) {
            return false;
        }

        anvil_mir_vreg_t uses[] = {lhs, rhs};
        bool added = mir_op == ANVIL_MIR_OP_FCMP ? anvil_mir_add_instr_imm_uses(lower->mir, mir_op, def, uses, 2, instr->fcmp_pred) : anvil_mir_add_instr(lower->mir, mir_op, def, uses, 2);
        if (!added) {
            return false;
        }
        return map_put(lower, instr->result, def);
    }

    if (instr->num_operands == 1 && map_unop(instr->op, &mir_op)) {
        if (instr->result && x86_needs_pair(instr->result->type)) {
            return lower_i64_unop_const(lower, instr);
        }
        anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t def = instr->result ? x86_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
        if (src == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG)
            return false;

        anvil_mir_vreg_t uses[] = {src};
        if (!anvil_mir_add_instr(lower->mir, mir_op, def, uses, 1))
            return false;
        return map_put(lower, instr->result, def);
    }

    if (instr->num_operands == 1 && map_cast(instr->op, &mir_op)) {
        return lower_cast(lower, instr, mir_op);
    }

    switch (instr->op) {
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
        bool is_i1 = instr->result->type->kind == ANVIL_TYPE_I1;
        anvil_mir_vreg_t loaded = is_i1 ? anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false) : def;
        if (loaded == ANVIL_MIR_NO_VREG)
            return false;
        anvil_mir_vreg_t uses[] = {ptr};
        bool ok = offset == 0 ? anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_LOAD, loaded, uses, 1) : anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_LOAD, loaded, uses, 1, offset);
        if (!ok) {
            return false;
        }
        if (is_i1) {
            anvil_mir_vreg_t one = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
            anvil_mir_vreg_t norm_uses[] = {loaded, one};
            if (one == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, one, 1) || !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_AND, def, norm_uses, 2))
                return false;
        }
        return map_put(lower, instr->result, def);
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
            anvil_mir_vreg_t one = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
            anvil_mir_vreg_t normalized = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
            anvil_mir_vreg_t norm_uses[] = {val, one};
            if (one == ANVIL_MIR_NO_VREG || normalized == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, one, 1) ||
                !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_AND, normalized, norm_uses, 2))
                return false;
            val = normalized;
        }
        anvil_mir_vreg_t uses[] = {val, ptr};
        if (offset == 0) {
            return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, uses, 2);
        }
        return anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, uses, 2, offset);
    }
    case ANVIL_OP_CALL:
        return lower_call(lower, instr);
    case ANVIL_OP_BR: {
        if (!instr->true_block)
            return false;
        anvil_mir_block_t target = block_get(lower, instr->true_block);
        if (target == ANVIL_MIR_NO_BLOCK)
            return false;
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
        if (cond == ANVIL_MIR_NO_VREG || true_block == ANVIL_MIR_NO_BLOCK || false_block == ANVIL_MIR_NO_BLOCK) {
            return false;
        }

        anvil_mir_block_t source_block = anvil_mir_current_block(lower->mir);
        anvil_mir_block_t true_target = true_block;
        anvil_mir_block_t false_target = false_block;
        anvil_mir_block_t true_edge = ANVIL_MIR_NO_BLOCK;
        anvil_mir_block_t false_edge = ANVIL_MIR_NO_BLOCK;

        if (block_starts_with_phi(instr->true_block)) {
            true_edge = create_phi_edge_block(lower, instr->parent, instr->true_block);
            if (true_edge == ANVIL_MIR_NO_BLOCK)
                return false;
            true_target = true_edge;
        }

        if (block_starts_with_phi(instr->false_block)) {
            false_edge = create_phi_edge_block(lower, instr->parent, instr->false_block);
            if (false_edge == ANVIL_MIR_NO_BLOCK)
                return false;
            false_target = false_edge;
        }

        if (!anvil_mir_set_current_block(lower->mir, source_block)) {
            return false;
        }

        if (!anvil_mir_add_cond_branch(lower->mir, cond, true_target, false_target)) {
            return false;
        }

        if (true_edge != ANVIL_MIR_NO_BLOCK && !emit_phi_edge_block(lower, instr->parent, instr->true_block, true_edge, true_block)) {
            return false;
        }

        if (false_edge != ANVIL_MIR_NO_BLOCK && !emit_phi_edge_block(lower, instr->parent, instr->false_block, false_edge, false_block)) {
            return false;
        }

        return anvil_mir_set_current_block(lower->mir, source_block);
    }
    case ANVIL_OP_SWITCH:
        return lower_switch(lower, instr);
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
    lower.func = func;
    lower.mir = anvil_mir_func_create(func->name);
    if (!lower.mir)
        return NULL;

    anvil_opt_cfg_t cfg;
    if (!anvil_opt_cfg_build(func, &cfg)) {
        anvil_mir_func_destroy(lower.mir);
        return NULL;
    }

    if (!create_mir_blocks(&lower) || !lower_params(&lower) || !prepare_phi_results(&lower)) {
        anvil_opt_cfg_destroy(&cfg);
        anvil_mir_func_destroy(lower.mir);
        free(lower.blocks);
        free(lower.values);
        free(lower.pairs);
        free(lower.wide_consts);
        free(lower.addr_offsets);
        return NULL;
    }

    for (size_t rank = 0; rank < cfg.count; rank++) {
        anvil_block_t *block = cfg.blocks[cfg.rpo[rank]];
        anvil_mir_block_t mir_block = block_get(&lower, block);
        if (mir_block == ANVIL_MIR_NO_BLOCK || !anvil_mir_set_current_block(lower.mir, mir_block)) {
            anvil_opt_cfg_destroy(&cfg);
            anvil_mir_func_destroy(lower.mir);
            free(lower.blocks);
            free(lower.values);
            free(lower.pairs);
            free(lower.wide_consts);
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
                free(lower.pairs);
                free(lower.wide_consts);
                free(lower.addr_offsets);
                return NULL;
            }
        }
    }

    free(lower.blocks);
    free(lower.values);
    free(lower.pairs);
    free(lower.wide_consts);
    free(lower.addr_offsets);
    anvil_opt_cfg_destroy(&cfg);
    return lower.mir;
}
