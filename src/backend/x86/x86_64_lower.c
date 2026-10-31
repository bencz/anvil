#include "x86_64_internal.h"
#include "../common/narrow_mir.h"
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
    const anvil_x64_abi_desc_t *desc;
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
    /* SRET result: in registers (sret_object holds it until RET) or through
     * the caller's hidden pointer (returned in RAX). */
    bool sret_in_regs;
    x64_agg_plan_t sret_plan;
    anvil_mir_vreg_t sret_object;
    /* Argument state after the named parameters (va_start). */
    x64_arg_state_t named_args;
    anvil_mir_vreg_t sret_pointer;
} x64_mir_lower_t;

static bool x64_type_is_fp(anvil_type_t *type)
{
    return type && (type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64);
}

static bool x64_type_is_void(anvil_type_t *type)
{
    return !type || type->kind == ANVIL_TYPE_VOID;
}

static bool x64_type_is_signed(anvil_type_t *type)
{
    return type && type->is_signed && !x64_type_is_fp(type);
}

static anvil_mir_reg_class_t x64_reg_class_for_type(anvil_type_t *type)
{
    return x64_type_is_fp(type) || (type && type->kind == ANVIL_TYPE_VECTOR) ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR;
}

static uint16_t x64_bits_for_type(anvil_type_t *type)
{
    if (!type)
        return 64;
    if (type->kind == ANVIL_TYPE_PTR)
        return 64;

    size_t size = anvil_type_size(type);
    if (size == 0)
        return 64;
    if (size > UINT16_MAX / 8)
        return 64;
    return (uint16_t)(size * 8);
}

static size_t x64_slot_bytes_for_type(anvil_type_t *type)
{
    size_t size = type ? anvil_type_size(type) : 8;
    return size ? size : 8;
}

static uint16_t x64_align_for_type(anvil_type_t *type)
{
    int align = type ? x64_type_align(type) : 8;
    if (align <= 0)
        align = 8;
    if (align > UINT16_MAX)
        align = UINT16_MAX;
    return (uint16_t)align;
}

static anvil_mir_vreg_t x64_add_vreg_for_type(x64_mir_lower_t *lower, anvil_type_t *type)
{
    if (type && type->kind == ANVIL_TYPE_VECTOR && !anvil_vector_operation_cost(lower->func->owner_ctx, ANVIL_OP_LOAD, type))
        return ANVIL_MIR_NO_VREG;

    return anvil_mir_add_vreg_typed(lower->mir, x64_reg_class_for_type(type), x64_bits_for_type(type), x64_type_is_signed(type));
}

static bool map_reserve(x64_mir_lower_t *lower, size_t needed)
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

static bool map_put(x64_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t vreg)
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

static anvil_mir_vreg_t map_get(x64_mir_lower_t *lower, anvil_value_t *value)
{
    for (size_t i = 0; i < lower->num_values; i++) {
        if (lower->values[i].value == value)
            return lower->values[i].vreg;
    }
    return ANVIL_MIR_NO_VREG;
}

static bool addr_map_reserve(x64_mir_lower_t *lower, size_t needed)
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

static bool addr_map_put(x64_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t base, int64_t offset)
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

static bool addr_map_get(x64_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *out_base, int64_t *out_offset)
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

static anvil_mir_block_t block_get(x64_mir_lower_t *lower, anvil_block_t *block)
{
    if (!block)
        return ANVIL_MIR_NO_BLOCK;
    for (size_t i = 0; i < lower->num_blocks; i++) {
        if (lower->blocks[i].block == block)
            return lower->blocks[i].mir_block;
    }
    return ANVIL_MIR_NO_BLOCK;
}

static bool create_mir_blocks(x64_mir_lower_t *lower)
{
    lower->num_blocks = lower->func->num_blocks;
    if (lower->num_blocks == 0)
        return true;

    lower->blocks = calloc(lower->num_blocks, sizeof(*lower->blocks));
    if (!lower->blocks)
        return false;

    size_t index = 0;
    for (anvil_block_t *block = lower->func->blocks; block; block = block->next) {
        anvil_mir_block_t mir_block = anvil_mir_block_for(lower->mir, block, index == 0);
        if (mir_block == ANVIL_MIR_NO_BLOCK)
            return false;

        lower->blocks[index].block = block;
        lower->blocks[index].mir_block = mir_block;
        index++;
    }

    return true;
}

/* ------------------------------------------------------------------
 * Aggregates passed (BYVAL) or returned (SRET) by value.
 * ------------------------------------------------------------------ */

/* Parts move through the shared helpers, which never emit shifts: those
 * would need the emitter's RAX/RCX scratch registers. */
static const anvil_agg_target_t X64_AGG_TARGET = {8, false};

static anvil_mir_vreg_t agg_frame_object(x64_mir_lower_t *lower, size_t size, size_t align)
{
    return anvil_agg_frame_object(lower->mir, size, align, X64_AGG_TARGET);
}

static anvil_mir_vreg_t agg_load_part(x64_mir_lower_t *lower, const x64_agg_part_t *part, anvil_mir_vreg_t base)
{
    return anvil_agg_load_part(lower->mir, part, base, X64_AGG_TARGET);
}

static bool agg_store_part(x64_mir_lower_t *lower, const x64_agg_part_t *part, anvil_mir_vreg_t value, anvil_mir_vreg_t base)
{
    return anvil_agg_store_part(lower->mir, part, value, base, X64_AGG_TARGET);
}

static x64_agg_part_t agg_chunk(size_t size, size_t offset)
{
    return anvil_agg_chunk(size, offset, X64_AGG_TARGET);
}

static bool agg_copy(x64_mir_lower_t *lower, anvil_mir_vreg_t from, anvil_mir_vreg_t to, size_t size)
{
    return anvil_agg_copy(lower->mir, from, to, size, X64_AGG_TARGET);
}

static uint16_t agg_part_bits(const x64_agg_part_t *part)
{
    return anvil_agg_part_bits(part, X64_AGG_TARGET);
}

/* An incoming argument as an ordinary virtual register.  Where it arrives
 * is the ABI module's decision (x64_assign_*). */
static anvil_mir_vreg_t lower_incoming_value(x64_mir_lower_t *lower, int reg, int64_t offset, anvil_mir_reg_class_t cls, uint16_t bits, bool is_signed)
{
    anvil_mir_vreg_t value = anvil_mir_add_vreg_typed(lower->mir, cls, bits, is_signed);
    if (value == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;
    if (reg < 0)
        return anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, value, offset) ? value : ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t incoming = anvil_mir_add_vreg_typed(lower->mir, cls, bits, is_signed);
    anvil_mir_vreg_t uses[] = {incoming};
    if (incoming == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->mir, incoming, reg) || !anvil_mir_set_live_in(lower->mir, incoming, true) ||
        !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, value, uses, 1))
        return ANVIL_MIR_NO_VREG;
    return value;
}

static bool lower_params(x64_mir_lower_t *lower)
{
    const anvil_x64_abi_desc_t *desc = lower->desc;
    x64_arg_state_t state = {0, 0, 0, 0};

    anvil_mir_block_t entry = block_get(lower, lower->func->blocks);
    if (entry == ANVIL_MIR_NO_BLOCK || !anvil_mir_set_current_block(lower->mir, entry)) {
        return false;
    }

    for (size_t i = 0; i < lower->func->num_params; i++) {
        anvil_value_t *param = lower->func->params[i];
        if (!param)
            return false;
        if (param->type->kind == ANVIL_TYPE_VECTOR)
            return false;

        anvil_param_attr_t attr = anvil_type_func_param_attr(lower->func->type, i);
        if (attr != ANVIL_PARAM_PLAIN) {
            x64_agg_plan_t plan;
            if (!x64_classify_aggregate(desc, param->type->data.pointee, attr == ANVIL_PARAM_SRET, &plan))
                return false;

            if (attr == ANVIL_PARAM_SRET && plan.placement == X64_AGG_REGS) {
                /* The result lives in a local object until RET loads it
                 * into the return registers; no argument slot is used. */
                anvil_mir_vreg_t object = agg_frame_object(lower, plan.size, plan.align);
                if (object == ANVIL_MIR_NO_VREG || !map_put(lower, param, object))
                    return false;
                lower->sret_in_regs = true;
                lower->sret_plan = plan;
                lower->sret_object = object;
                continue;
            }

            if (attr == ANVIL_PARAM_BYVAL && plan.placement != X64_AGG_REFERENCE) {
                /* The callee's own copy, filled from registers or from the
                 * caller's stack argument area. */
                anvil_mir_vreg_t object = agg_frame_object(lower, plan.size, plan.align);
                int regs[2];
                x64_arg_loc_t loc = x64_assign_aggregate(desc, &state, &plan, regs);
                if (object == ANVIL_MIR_NO_VREG)
                    return false;
                if (loc.reg >= 0) {
                    for (unsigned part = 0; part < plan.num_parts; part++) {
                        const x64_agg_part_t *p = &plan.parts[part];
                        anvil_mir_vreg_t value = lower_incoming_value(lower, regs[part], 0, p->fp ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR, agg_part_bits(p), false);
                        if (value == ANVIL_MIR_NO_VREG || !agg_store_part(lower, p, value, object))
                            return false;
                    }
                } else {
                    for (size_t offset = 0; offset < plan.size; offset += 8) {
                        x64_agg_part_t chunk = agg_chunk(plan.size, offset);
                        anvil_mir_vreg_t value = lower_incoming_value(lower, -1, loc.offset + (int64_t)offset, ANVIL_MIR_REG_GPR, 64, false);
                        if (value == ANVIL_MIR_NO_VREG || !agg_store_part(lower, &chunk, value, object))
                            return false;
                    }
                }
                if (!map_put(lower, param, object))
                    return false;
                continue;
            }
            /* X64_AGG_REFERENCE / X64_AGG_HIDDEN_POINTER: an ordinary pointer
             * argument; a hidden result pointer is also returned in RAX. */
        }

        x64_arg_loc_t loc = x64_assign_scalar(desc, &state, x64_type_is_fp(param->type));
        anvil_mir_vreg_t value = lower_incoming_value(lower, loc.reg, loc.offset, x64_type_is_fp(param->type) ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR,
                                                      x64_bits_for_type(param->type), x64_type_is_signed(param->type));
        if (value == ANVIL_MIR_NO_VREG || !map_put(lower, param, value))
            return false;
        if (attr == ANVIL_PARAM_SRET)
            lower->sret_pointer = value;
    }

    lower->named_args = state;
    return true;
}

static bool prepare_phi_results(x64_mir_lower_t *lower)
{
    for (anvil_block_t *block = lower->func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_PHI)
                break;
            if (!instr->result)
                return false;

            anvil_mir_vreg_t vreg = x64_add_vreg_for_type(lower, instr->result->type);
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

static anvil_mir_vreg_t lower_value(x64_mir_lower_t *lower, anvil_value_t *value);
static bool lower_add_const_offset(x64_mir_lower_t *lower, anvil_mir_vreg_t base, int64_t offset, anvil_mir_vreg_t *out_ptr);

static anvil_mir_vreg_t lower_const_value(x64_mir_lower_t *lower, anvil_value_t *value)
{
    anvil_mir_vreg_t vreg = x64_add_vreg_for_type(lower, value->type);
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
        imm = float_bits_as_i64(value->data.f, x64_bits_for_type(value->type));
        break;
    default:
        return ANVIL_MIR_NO_VREG;
    }

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, vreg, imm)) {
        return ANVIL_MIR_NO_VREG;
    }
    return vreg;
}

/* The last instruction names `source` (a global or function): mark it to go
 * through the GOT/PLT when the relocation model requires. */
static bool x64_mark_symbol_access(x64_mir_lower_t *lower, const anvil_value_t *source)
{
    anvil_ctx_t *ctx = lower->func->parent ? lower->func->parent->ctx : NULL;
    return !source || !anvil_symbol_uses_got(ctx, source) ||
           anvil_mir_set_instr_symbol_got(lower->mir, anvil_mir_num_instrs(lower->mir) - 1, true);
}

static anvil_mir_vreg_t lower_symbol_address(x64_mir_lower_t *lower, const char *symbol, const anvil_value_t *source)
{
    if (!symbol || !symbol[0])
        return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t vreg = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 64, false);
    if (vreg == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;

    if (!anvil_mir_add_instr_symbol(lower->mir, ANVIL_MIR_OP_SYMBOL_ADDR, vreg, NULL, 0, symbol) || !x64_mark_symbol_access(lower, source)) {
        return ANVIL_MIR_NO_VREG;
    }
    return vreg;
}

static anvil_mir_vreg_t lower_reloc_address(x64_mir_lower_t *lower, anvil_value_t *value)
{
    const char *symbol = value && value->data.reloc.symbol ? value->data.reloc.symbol->name : NULL;
    if (!symbol || !symbol[0])
        return ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t vreg = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 64, false);
    if (vreg == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_symbol_imm(lower->mir, ANVIL_MIR_OP_SYMBOL_ADDR, vreg, NULL, 0, symbol, value->data.reloc.addend) ||
        !x64_mark_symbol_access(lower, value->data.reloc.symbol))
        return ANVIL_MIR_NO_VREG;
    return vreg;
}

static anvil_mir_vreg_t lower_string_address(x64_mir_lower_t *lower, anvil_value_t *value)
{
    const char *label = NULL;
    if (anvil_mir_add_string_literal(lower->mir, value->data.str, &label) < 0 || !label) {
        return ANVIL_MIR_NO_VREG;
    }
    return lower_symbol_address(lower, label, NULL);
}

static anvil_mir_vreg_t lower_value(x64_mir_lower_t *lower, anvil_value_t *value)
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
            return lower_symbol_address(lower, value->data.func->name, value);
        }
        return lower_symbol_address(lower, value->name, value);
    case ANVIL_VAL_GLOBAL:
        return lower_symbol_address(lower, value->name, value);
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

/* Physical return register of the given SysV/Win64 part. */
static int agg_return_reg(const anvil_x64_abi_desc_t *desc, const x64_agg_plan_t *plan, unsigned part)
{
    bool fp = plan->parts[part].fp;
    unsigned before = 0;
    for (unsigned i = 0; i < part; i++)
        before += plan->parts[i].fp == fp;
    if (fp)
        return before == 0 ? desc->fp_ret_reg : desc->fp_ret_reg + 1;
    return before == 0 ? desc->int_ret_reg : desc->int_ret_hi_reg;
}

static bool add_return(x64_mir_lower_t *lower, anvil_value_t *value)
{
    if (!value && lower->sret_in_regs) {
        /* Load the result object into its return registers; the extra
         * register stays live up to RET through KEEPALIVE. */
        const x64_agg_plan_t *plan = &lower->sret_plan;
        anvil_mir_vreg_t fixed[2] = {ANVIL_MIR_NO_VREG, ANVIL_MIR_NO_VREG};
        for (unsigned part = 0; part < plan->num_parts; part++) {
            const x64_agg_part_t *p = &plan->parts[part];
            anvil_mir_vreg_t loaded = agg_load_part(lower, p, lower->sret_object);
            fixed[part] = anvil_mir_add_vreg_typed(lower->mir, p->fp ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR, agg_part_bits(p), false);
            anvil_mir_vreg_t uses[] = {loaded};
            int reg = agg_return_reg(lower->desc, plan, part);
            if (loaded == ANVIL_MIR_NO_VREG || fixed[part] == ANVIL_MIR_NO_VREG || reg < 0 ||
                !anvil_mir_set_fixed_reg(lower->mir, fixed[part], reg) ||
                !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, fixed[part], uses, 1))
                return false;
        }
        if (plan->num_parts == 2 && !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_KEEPALIVE, ANVIL_MIR_NO_VREG, &fixed[1], 1))
            return false;
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, &fixed[0], 1);
    }
    if (!value && lower->sret_pointer != ANVIL_MIR_NO_VREG) {
        /* A hidden result pointer is returned in RAX. */
        anvil_mir_vreg_t uses[] = {lower->sret_pointer};
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, uses, 1);
    }
    if (!value) {
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, NULL, 0);
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

static bool lower_phi_copies_for_edge(x64_mir_lower_t *lower, anvil_block_t *src_block, anvil_block_t *dest_block)
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

static anvil_mir_block_t create_phi_edge_block(x64_mir_lower_t *lower, anvil_block_t *src_block, anvil_block_t *dest_block)
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

static bool emit_phi_edge_block(x64_mir_lower_t *lower, anvil_block_t *src_block, anvil_block_t *dest_block, anvil_mir_block_t edge_block, anvil_mir_block_t dest_mir_block)
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

static bool prepare_phi_aware_target(x64_mir_lower_t *lower, anvil_block_t *src_block, anvil_block_t *dest_block, anvil_mir_block_t *out_target, pending_phi_edge_t **edges, size_t *num_edges,
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

static bool emit_pending_phi_edges(x64_mir_lower_t *lower, anvil_block_t *src_block, pending_phi_edge_t *edges, size_t num_edges)
{
    for (size_t i = 0; i < num_edges; i++) {
        if (!emit_phi_edge_block(lower, src_block, edges[i].dest_block, edges[i].edge_block, edges[i].dest_mir_block)) {
            return false;
        }
    }
    return true;
}

static anvil_mir_block_t create_switch_chain_block(x64_mir_lower_t *lower, anvil_block_t *src_block, size_t case_index)
{
    const char *src_name = (src_block && src_block->name) ? src_block->name : "anon";

    char name[256];
    snprintf(name, sizeof(name), "%s_switch_case_%zu_%zu", src_name, case_index, lower->num_edge_blocks++);
    return anvil_mir_add_block(lower->mir, name);
}

static int x64_call_num_vector_args(anvil_type_t *fn_type, anvil_instr_t *instr)
{
    int count = 0;
    for (size_t i = 1; i < instr->num_operands; i++) {
        anvil_value_t *arg = instr->operands[i];
        if (arg && x64_type_is_fp(arg->type))
            count++;
    }
    (void)fn_type;
    return count < 8 ? count : 8;
}

static bool lower_call(x64_mir_lower_t *lower, anvil_instr_t *instr)
{
    const anvil_x64_abi_desc_t *desc = NULL;
    if (instr->call_cc == ANVIL_CC_WIN64) {
        desc = anvil_x64_get_abi_desc(ANVIL_ABI_WIN64);
    } else if (instr->call_cc == ANVIL_CC_SYSV && !lower->desc->is_win64) {
        /* Darwin uses the SysV register convention with Mach-O spelling. */
        desc = lower->desc;
    }
    if (!desc || desc->is_win64 != lower->desc->is_win64)
        return false;
    if (instr->num_operands == 0)
        return false;

    anvil_value_t *callee = instr->operands[0];
    anvil_type_t *fn_type = call_func_type(callee);
    if (!fn_type)
        return false;
    if (fn_type->data.func.ret->kind == ANVIL_TYPE_VECTOR)
        return false;

    for (size_t argument = 1; argument < instr->num_operands; argument++) {
        if (instr->operands[argument]->type->kind == ANVIL_TYPE_VECTOR)
            return false;
    }

    bool direct_call = call_is_direct_symbol(callee);
    const char *symbol = direct_call ? call_symbol(callee) : NULL;
    if (direct_call && !symbol)
        return false;

    bool is_variadic = fn_type->data.func.variadic;
    int vector_args = is_variadic ? x64_call_num_vector_args(fn_type, instr) : 0;

    size_t num_args = instr->num_operands - 1;
    if (num_args > (SIZE_MAX - (direct_call ? 0u : 1u)) / 2)
        return false;
    /* Win64 variadic floating arguments occupy both their positional XMM
       register and the corresponding integer register. */
    size_t max_call_uses = num_args * 2 + (direct_call ? 0 : 1);
    anvil_mir_vreg_t *call_uses = NULL;
    if (max_call_uses > 0) {
        call_uses = calloc(max_call_uses, sizeof(*call_uses));
        if (!call_uses)
            return false;
    }

    x64_arg_state_t state = {0, 0, 0, 0};
    size_t num_call_uses = direct_call ? 0 : 1;     /* [0]: call target */
    bool ok = true;
    /* Aggregate result returned in registers into the caller's object. */
    bool sret_in_regs = false;
    x64_agg_plan_t sret_plan;
    anvil_mir_vreg_t sret_object = ANVIL_MIR_NO_VREG;

    for (size_t i = 0; ok && i < num_args; i++) {
        anvil_value_t *arg = instr->operands[i + 1];
        anvil_mir_vreg_t src = lower_value(lower, arg);
        if (src == ANVIL_MIR_NO_VREG) {
            ok = false;
            break;
        }

        anvil_param_attr_t attr = anvil_call_arg_attr(instr, fn_type, i);
        if (attr != ANVIL_PARAM_PLAIN) {
            x64_agg_plan_t plan;
            if (!x64_classify_aggregate(desc, arg->type->data.pointee, attr == ANVIL_PARAM_SRET, &plan)) {
                ok = false;
                break;
            }
            if (attr == ANVIL_PARAM_SRET && plan.placement == X64_AGG_REGS) {
                /* Not passed: the result registers are stored into it. */
                sret_in_regs = true;
                sret_plan = plan;
                sret_object = src;
                continue;
            }
            if (attr == ANVIL_PARAM_BYVAL && plan.placement == X64_AGG_REFERENCE) {
                /* A pointer to a 16-byte aligned caller copy. */
                anvil_mir_vreg_t copy = agg_frame_object(lower, plan.size, 16);
                if (copy == ANVIL_MIR_NO_VREG || !agg_copy(lower, src, copy, plan.size)) {
                    ok = false;
                    break;
                }
                src = copy;
            } else if (attr == ANVIL_PARAM_BYVAL) {
                int regs[2];
                x64_arg_loc_t loc = x64_assign_aggregate(desc, &state, &plan, regs);
                if (loc.reg >= 0) {
                    anvil_mir_vreg_t values[2];
                    for (unsigned part = 0; ok && part < plan.num_parts; part++) {
                        values[part] = agg_load_part(lower, &plan.parts[part], src);
                        ok = values[part] != ANVIL_MIR_NO_VREG;
                    }
                    for (unsigned part = 0; ok && part < plan.num_parts; part++) {
                        const x64_agg_part_t *p = &plan.parts[part];
                        anvil_mir_vreg_t fixed = anvil_mir_add_vreg_typed(lower->mir, p->fp ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR, agg_part_bits(p), false);
                        anvil_mir_vreg_t uses[] = {values[part]};
                        ok = fixed != ANVIL_MIR_NO_VREG && anvil_mir_set_fixed_reg(lower->mir, fixed, regs[part]) &&
                             anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, fixed, uses, 1);
                        call_uses[num_call_uses++] = fixed;
                    }
                } else {
                    for (size_t offset = 0; ok && offset < plan.size; offset += 8) {
                        x64_agg_part_t chunk = agg_chunk(plan.size, offset);
                        anvil_mir_vreg_t value = agg_load_part(lower, &chunk, src);
                        anvil_mir_vreg_t stack_use[] = {value};
                        ok = value != ANVIL_MIR_NO_VREG &&
                             anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, stack_use, 1, loc.offset + (int64_t)offset);
                    }
                }
                continue;
            }
            /* Reference copies and hidden result pointers travel as an
             * ordinary pointer argument below. */
        }

        x64_arg_loc_t loc = x64_assign_scalar(desc, &state, x64_type_is_fp(arg->type));
        const anvil_mir_vreg_info_t *arg_info = anvil_mir_get_vreg_info(lower->mir, src);
        bool widen = desc->caller_extends_narrow_args && arg_info && arg_info->reg_class == ANVIL_MIR_REG_GPR && arg_info->size_bits < 32;
        if (widen) {
            src = anvil_mir_extend_gpr(lower->mir, src, 32, arg->type->is_signed && arg->type->kind != ANVIL_TYPE_I1);
            if (src == ANVIL_MIR_NO_VREG) {
                ok = false;
                break;
            }
        }
        if (loc.reg < 0) {
            anvil_mir_vreg_t stack_use[] = {src};
            ok = anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, stack_use, 1, loc.offset);
            continue;
        }

        anvil_mir_vreg_t fixed_arg = widen ? anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, false) : x64_add_vreg_for_type(lower, arg->type);
        anvil_mir_vreg_t copy_uses[] = {src};
        if (fixed_arg == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->mir, fixed_arg, loc.reg) ||
            !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, fixed_arg, copy_uses, 1)) {
            ok = false;
            break;
        }
        call_uses[num_call_uses++] = fixed_arg;
        /* Positional ABIs also pass a variadic callee's floating arguments
         * in the slot's integer register. */
        if (is_variadic && loc.shadow_reg >= 0) {
            const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->mir, src);
            if (!src_info || (src_info->size_bits != 32 && src_info->size_bits != 64)) {
                ok = false;
                break;
            }
            anvil_mir_vreg_t duplicate = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, src_info->size_bits, false);
            anvil_mir_vreg_t duplicate_uses[] = {src};
            if (duplicate == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->mir, duplicate, loc.shadow_reg) ||
                !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_BITCAST, duplicate, duplicate_uses, 1)) {
                ok = false;
                break;
            }
            call_uses[num_call_uses++] = duplicate;
        }
    }

    /* The target is loaded last, next to the call. */
    if (ok && !direct_call) {
        anvil_mir_vreg_t target_src = lower_value(lower, callee);
        anvil_mir_vreg_t target_fixed = x64_add_vreg_for_type(lower, callee->type);
        anvil_mir_vreg_t target_uses[] = {target_src};
        ok = target_src != ANVIL_MIR_NO_VREG && target_fixed != ANVIL_MIR_NO_VREG &&
             anvil_mir_set_fixed_reg(lower->mir, target_fixed, desc->indirect_call_reg) &&
             anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, target_fixed, target_uses, 1);
        call_uses[0] = target_fixed;
    }

    if (!ok) {
        free(call_uses);
        return false;
    }
    /* SysV variadic calls report the vector registers used, including the
     * SSE parts of aggregates. */
    if (is_variadic && !desc->is_win64)
        vector_args = state.fpr < 8 ? (int)state.fpr : 8;

    anvil_mir_vreg_t call_def = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t sret_second = ANVIL_MIR_NO_VREG;
    if (sret_in_regs) {
        const x64_agg_part_t *p = &sret_plan.parts[0];
        call_def = anvil_mir_add_vreg_typed(lower->mir, p->fp ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR, agg_part_bits(p), false);
        if (call_def == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->mir, call_def, agg_return_reg(desc, &sret_plan, 0))) {
            free(call_uses);
            return false;
        }
    } else if (instr->result && !x64_type_is_void(instr->result->type)) {
        call_def = x64_add_vreg_for_type(lower, instr->result->type);
        int ret_reg = x64_type_is_fp(instr->result->type) ? desc->fp_ret_reg : desc->int_ret_reg;
        if (call_def == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->mir, call_def, ret_reg)) {
            free(call_uses);
            return false;
        }
    }

    bool emit_ok = anvil_mir_add_call(lower->mir, call_def, call_uses, num_call_uses, symbol, instr->call_cc, is_variadic, vector_args) &&
                   (!symbol || x64_mark_symbol_access(lower, callee));
    free(call_uses);
    if (!emit_ok)
        return false;

    size_t call_index = anvil_mir_num_instrs(lower->mir) - 1;
    if (!anvil_mir_set_instr_clobbers(lower->mir, call_index, ANVIL_MIR_REG_GPR, desc->call_gpr_clobbers) ||
        !anvil_mir_set_instr_clobbers(lower->mir, call_index, ANVIL_MIR_REG_FPR, desc->call_fpr_clobbers))
        return false;

    if (sret_in_regs) {
        /* The second result register is defined by the same call. */
        if (sret_plan.num_parts == 2) {
            const x64_agg_part_t *p = &sret_plan.parts[1];
            sret_second = anvil_mir_add_vreg_typed(lower->mir, p->fp ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR, agg_part_bits(p), false);
            if (sret_second == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->mir, sret_second, agg_return_reg(desc, &sret_plan, 1)) ||
                !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_CALL_RESULT, sret_second, NULL, 0))
                return false;
        }
        anvil_mir_vreg_t results[2] = {call_def, sret_second};
        for (unsigned part = 0; part < sret_plan.num_parts; part++) {
            const x64_agg_part_t *p = &sret_plan.parts[part];
            anvil_mir_vreg_t local = anvil_mir_add_vreg_typed(lower->mir, p->fp ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR, agg_part_bits(p), false);
            anvil_mir_vreg_t uses[] = {results[part]};
            if (local == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, local, uses, 1))
                return false;
            results[part] = local;
        }
        for (unsigned part = 0; part < sret_plan.num_parts; part++)
            if (!agg_store_part(lower, &sret_plan.parts[part], results[part], sret_object))
                return false;
        return true;
    }

    if (instr->result && call_def != ANVIL_MIR_NO_VREG) {
        anvil_mir_vreg_t local_result = x64_add_vreg_for_type(lower, instr->result->type);
        if (local_result == ANVIL_MIR_NO_VREG)
            return false;

        anvil_mir_vreg_t copy_uses[] = {call_def};
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, local_result, copy_uses, 1)) {
            return false;
        }
        return map_put(lower, instr->result, local_result);
    }
    return true;
}

static anvil_mir_vreg_t lower_widen_gpr_to_64(x64_mir_lower_t *lower, anvil_mir_vreg_t src, bool sign_extend)
{
    const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->mir, src);
    if (!src_info || src_info->reg_class != ANVIL_MIR_REG_GPR) {
        return ANVIL_MIR_NO_VREG;
    }
    if (src_info->size_bits >= 64)
        return src;

    anvil_mir_vreg_t wide = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 64, sign_extend);
    if (wide == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t uses[] = {src};
    anvil_mir_opcode_t op = sign_extend ? ANVIL_MIR_OP_SEXT : ANVIL_MIR_OP_ZEXT;
    if (!anvil_mir_add_instr(lower->mir, op, wide, uses, 1)) {
        return ANVIL_MIR_NO_VREG;
    }
    return wide;
}

static anvil_mir_vreg_t lower_resize_gpr(x64_mir_lower_t *lower, anvil_mir_vreg_t src, uint16_t target_bits)
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

static bool lower_match_binary_operand_sizes(x64_mir_lower_t *lower, anvil_mir_vreg_t *lhs, anvil_mir_vreg_t *rhs, anvil_mir_vreg_t def)
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

static bool lower_alloca(x64_mir_lower_t *lower, anvil_instr_t *instr)
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

    anvil_mir_vreg_t ptr = x64_add_vreg_for_type(lower, instr->result->type);
    if (ptr == ANVIL_MIR_NO_VREG)
        return false;

    if (instr->num_operands == 0) {
        int slot = anvil_mir_add_frame_slot(lower->mir, x64_slot_bytes_for_type(element_type), x64_align_for_type(element_type));
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
    count = lower_widen_gpr_to_64(lower, count, false);
    if (count == ANVIL_MIR_NO_VREG)
        return false;

    int64_t elem_size = x64_type_size(element_type);
    if (elem_size <= 0)
        elem_size = 1;
    anvil_mir_vreg_t uses[] = {count};
    if (!anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_DYN_ALLOCA, ptr, uses, 1, elem_size)) {
        return false;
    }
    return map_put(lower, instr->result, ptr);
}

static bool lower_add_const_offset(x64_mir_lower_t *lower, anvil_mir_vreg_t base, int64_t offset, anvil_mir_vreg_t *out_ptr)
{
    if (offset == 0) {
        *out_ptr = base;
        return true;
    }

    anvil_mir_vreg_t off = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t dst = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 64);
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

static bool lower_gep(x64_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands < 2 || !instr->result || !instr->aux_type)
        return false;

    anvil_mir_vreg_t current = lower_value(lower, instr->operands[0]);
    if (current == ANVIL_MIR_NO_VREG)
        return false;
    current = lower_widen_gpr_to_64(lower, current, false);
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
        anvil_mir_vreg_t index = lower_value(lower, index_value);
        if (index == ANVIL_MIR_NO_VREG)
            return false;
        index = lower_widen_gpr_to_64(lower, index, index_value->type->is_signed);
        if (index == ANVIL_MIR_NO_VREG)
            return false;

        anvil_mir_vreg_t scaled = index;
        if (elem_size != 1) {
            anvil_mir_vreg_t scale = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 64);
            scaled = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 64);
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

        anvil_mir_vreg_t next = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 64);
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

static bool lower_struct_gep(x64_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands < 1 || !instr->result)
        return false;

    anvil_mir_vreg_t base = lower_value(lower, instr->operands[0]);
    if (base == ANVIL_MIR_NO_VREG)
        return false;
    base = lower_widen_gpr_to_64(lower, base, false);
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

static bool lower_cast(x64_mir_lower_t *lower, anvil_instr_t *instr, anvil_mir_opcode_t mir_op)
{
    if (instr->num_operands != 1 || !instr->result)
        return false;
    /* A pointer reinterpreted as another pointer is the same value. */
    if (anvil_cast_keeps_value(instr)) {
        anvil_mir_vreg_t same = lower_value(lower, instr->operands[0]);
        return same != ANVIL_MIR_NO_VREG && map_put(lower, instr->result, same);
    }
    anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t def = x64_add_vreg_for_type(lower, instr->result->type);
    if (src == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG)
        return false;
    /* Narrow integers convert from their extended value. */
    src = anvil_mir_int_to_fp_source(lower->mir, mir_op, src, 32);
    if (src == ANVIL_MIR_NO_VREG)
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

    if (mir_op == ANVIL_MIR_OP_BITCAST) {
        const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->mir, src);
        const anvil_mir_vreg_info_t *dst_info = anvil_mir_get_vreg_info(lower->mir, def);
        if (!src_info || !dst_info)
            return false;
        if (src_info->size_bits != dst_info->size_bits) {
            if (instr->op != ANVIL_OP_PTRTOINT && instr->op != ANVIL_OP_INTTOPTR) {
                return false;
            }
            mir_op = src_info->size_bits < dst_info->size_bits ? ANVIL_MIR_OP_ZEXT : ANVIL_MIR_OP_TRUNC;
        }
    }

    anvil_mir_vreg_t uses[] = {src};
    if (!anvil_mir_add_instr(lower->mir, mir_op, def, uses, 1))
        return false;
    return map_put(lower, instr->result, def);
}

static bool lower_select(x64_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands != 3 || !instr->result)
        return false;
    anvil_mir_vreg_t cond = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t then_v = lower_value(lower, instr->operands[1]);
    anvil_mir_vreg_t else_v = lower_value(lower, instr->operands[2]);
    anvil_mir_vreg_t def = x64_add_vreg_for_type(lower, instr->result->type);
    if (cond == ANVIL_MIR_NO_VREG || then_v == ANVIL_MIR_NO_VREG || else_v == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) {
        return false;
    }

    /* General registers: one conditional move (both values exist already). */
    const anvil_mir_vreg_info_t *def_info = anvil_mir_get_vreg_info(lower->mir, def);
    if (def_info && def_info->reg_class == ANVIL_MIR_REG_GPR) {
        anvil_mir_vreg_t uses[] = {cond, then_v, else_v};
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_SELECT, def, uses, 3) && map_put(lower, instr->result, def);
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

static bool lower_memory_address(x64_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *out_base, int64_t *out_offset)
{
    anvil_mir_vreg_t base = ANVIL_MIR_NO_VREG;
    int64_t offset = 0;
    if (addr_map_get(lower, value, &base, &offset)) {
        base = lower_widen_gpr_to_64(lower, base, false);
        if (base == ANVIL_MIR_NO_VREG)
            return false;
        *out_base = base;
        *out_offset = offset;
        return true;
    }

    base = lower_value(lower, value);
    if (base == ANVIL_MIR_NO_VREG)
        return false;
    base = lower_widen_gpr_to_64(lower, base, false);
    if (base == ANVIL_MIR_NO_VREG)
        return false;

    *out_base = base;
    *out_offset = 0;
    return true;
}

static bool lower_switch(x64_mir_lower_t *lower, anvil_instr_t *instr)
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

static bool lower_instr(x64_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->op == ANVIL_OP_NOP)
        return true;
    if (instr->op == ANVIL_OP_PHI)
        return true;

    /* Vector arithmetic and broadcasts, and the scalar operations the
     * target's table prices (FMA, FSQRT), where the target has them. */
    anvil_vector_mir_form_t vector;
    if (anvil_vector_mir_form(lower->func->owner_ctx, instr, &vector)) {
        anvil_mir_vreg_t uses[ANVIL_VECTOR_MAX_USES] = {ANVIL_MIR_NO_VREG, ANVIL_MIR_NO_VREG, ANVIL_MIR_NO_VREG};
        for (size_t operand = 0; operand < vector.num_uses; operand++)
            uses[operand] = lower_value(lower, instr->operands[operand]);
        anvil_mir_vreg_t def = vector.supported ? x64_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
        return anvil_vector_mir_emit(lower->mir, &vector, def, uses) && map_put(lower, instr->result, def);
    }

    anvil_mir_opcode_t mir_op;
    if (instr->num_operands == 2 && map_binop(instr->op, &mir_op)) {
        anvil_mir_vreg_t lhs = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t rhs = lower_value(lower, instr->operands[1]);
        anvil_mir_vreg_t def = instr->result ? x64_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
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
        anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t def = instr->result ? x64_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
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
    case ANVIL_OP_ATOMIC_LOAD:
    case ANVIL_OP_ATOMIC_STORE:
    case ANVIL_OP_ATOMIC_RMW:
    case ANVIL_OP_ATOMIC_CMPXCHG:
    case ANVIL_OP_ATOMIC_FENCE: {
        anvil_mir_vreg_t uses[3];
        for (size_t operand = 0; operand < instr->num_operands; operand++) {
            uses[operand] = lower_value(lower, instr->operands[operand]);
            if (uses[operand] == ANVIL_MIR_NO_VREG)
                return false;
        }

        anvil_mir_vreg_t def = instr->result ? x64_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
        if ((instr->result && def == ANVIL_MIR_NO_VREG) || !anvil_mir_add_atomic(lower->mir, instr->op, def, uses, instr->num_operands, &instr->atomic))
            return false;

        uint64_t scratch = (UINT64_C(1) << X64_RAX) | (UINT64_C(1) << X64_RCX) | (UINT64_C(1) << X64_RDX) | (UINT64_C(1) << X64_R11);
        if (!anvil_mir_set_instr_clobbers(lower->mir, anvil_mir_num_instrs(lower->mir) - 1, ANVIL_MIR_REG_GPR, scratch))
            return false;

        return !instr->result || map_put(lower, instr->result, def);
    }

    case ANVIL_OP_VA_START: {
        if (!instr->result || !lower->func->type->data.func.variadic || lower->func->num_params > INT_MAX / 8)
            return false;

        anvil_mir_vreg_t def = x64_add_vreg_for_type(lower, instr->result->type);
        if (!lower->desc->is_win64) {
            /* Registers and stack bytes the named parameters used. */
            size_t gpr = lower->named_args.gpr;
            size_t fpr = lower->named_args.fpr;
            size_t stack = (size_t)lower->named_args.stack;

            int slot = anvil_mir_add_frame_slot(lower->mir, 208, 16);
            size_t index = anvil_mir_num_instrs(lower->mir);
            return def != ANVIL_MIR_NO_VREG && slot >= 0 && anvil_mir_add_va_start(lower->mir, def, slot, (unsigned)(gpr < 6 ? gpr : 6), (unsigned)(fpr < 8 ? fpr : 8), stack) &&
                   anvil_mir_set_instr_clobbers(lower->mir, index, ANVIL_MIR_REG_GPR, UINT64_C(1) << X64_R11) && map_put(lower, instr->result, def);
        }

        /* Win64: the argument slots the named parameters used (a hidden
         * result pointer occupies one, a register-returned SRET none). */
        return def != ANVIL_MIR_NO_VREG && anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_VA_START, def, (int64_t)lower->named_args.position) &&
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
        if (instr->num_operands != 1 || !instr->result)
            return false;
        anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
        int64_t offset = 0;
        if (!lower_memory_address(lower, instr->operands[0], &ptr, &offset)) {
            return false;
        }
        anvil_mir_vreg_t def = x64_add_vreg_for_type(lower, instr->result->type);
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
        anvil_mir_vreg_t val = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
        int64_t offset = 0;
        if (!lower_memory_address(lower, instr->operands[1], &ptr, &offset)) {
            return false;
        }
        if (val == ANVIL_MIR_NO_VREG || ptr == ANVIL_MIR_NO_VREG)
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

static anvil_abi_t x64_lower_abi(anvil_func_t *func)
{
    anvil_abi_t abi = ANVIL_ABI_DEFAULT;
    if (func && func->parent && func->parent->ctx) {
        abi = func->parent->ctx->abi;
    }
    if (func && func->type && func->type->kind == ANVIL_TYPE_FUNC) {
        if (func->type->data.func.cc == ANVIL_CC_WIN64)
            abi = ANVIL_ABI_WIN64;
        else if (func->type->data.func.cc == ANVIL_CC_SYSV && abi != ANVIL_ABI_DARWIN)
            abi = ANVIL_ABI_SYSV;
    }
    if (abi == ANVIL_ABI_DEFAULT)
        abi = ANVIL_ABI_SYSV;
    return abi;
}

anvil_mir_func_t *anvil_x86_64_lower_func_to_mir(anvil_func_t *func)
{
    if (!func || func->is_declaration || !func->type || func->type->kind != ANVIL_TYPE_FUNC || (func->type->data.func.cc != ANVIL_CC_SYSV && func->type->data.func.cc != ANVIL_CC_WIN64))
        return NULL;

    const anvil_x64_abi_desc_t *desc = anvil_x64_get_abi_desc(x64_lower_abi(func));
    if (!desc)
        return NULL;
    if (func->type->data.func.ret->kind == ANVIL_TYPE_VECTOR)
        return NULL;

    x64_mir_lower_t lower;
    memset(&lower, 0, sizeof(lower));
    lower.sret_object = ANVIL_MIR_NO_VREG;
    lower.sret_pointer = ANVIL_MIR_NO_VREG;
    lower.desc = desc;
    lower.func = func;
    lower.mir = anvil_mir_func_create(func->name);
    anvil_mir_func_set_linkage(lower.mir, func->linkage);
    anvil_mir_func_set_cpu_features(lower.mir, anvil_ctx_get_cpu_features(func->owner_ctx));
    if (!lower.mir)
        return NULL;

    anvil_opt_cfg_t cfg;
    if (!anvil_opt_cfg_build(func, &cfg)) {
        anvil_mir_func_destroy(lower.mir);
        return NULL;
    }

    if (!anvil_opt_cfg_build_layout(&cfg) || !create_mir_blocks(&lower) || !lower_params(&lower) || !prepare_phi_results(&lower)) {
        anvil_opt_cfg_destroy(&cfg);
        anvil_mir_func_destroy(lower.mir);
        free(lower.blocks);
        free(lower.values);
        free(lower.addr_offsets);
        return NULL;
    }

    for (size_t rank = 0; rank < cfg.count; rank++) {
        anvil_block_t *block = cfg.blocks[cfg.layout[rank]];
        anvil_mir_block_t mir_block = block_get(&lower, block);
        if (mir_block == ANVIL_MIR_NO_BLOCK || !anvil_mir_set_current_block(lower.mir, mir_block)) {
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
