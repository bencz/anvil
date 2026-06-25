/*
 * ANVIL - x86-64 lowering to target-independent MachineIR.
 *
 * x86-64 follows the shared MachineIR/regalloc path. This file lowers source IR
 * into MachineIR, models ABI register and stack constraints through a target
 * descriptor, runs allocation/spill materialization, and emits x86-64 assembly
 * from the allocated machine instructions.
 */

#include "anvil/anvil_x86_64_mir.h"
#include "anvil/anvil_internal.h"
#include "x86_64_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int x64_sysv_int_args[] = { 7, 6, 2, 1, 8, 9 };
static const int x64_sysv_fp_args[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static const int x64_sysv_alloc_gpr[] = { 3, 12, 13, 14 };
static const int x64_sysv_scratch_gpr[] = { 10, 11, 15 };
static const int x64_sysv_scratch_fpr[] = { 8, 9, 10 };

static const int x64_win64_int_args[] = { 1, 2, 8, 9 };
static const int x64_win64_fp_args[] = { 0, 1, 2, 3 };
static const int x64_win64_alloc_gpr[] = { 3, 7, 6, 12, 13, 14 };
static const int x64_win64_alloc_fpr[] = { 6, 7, 8, 9, 10, 11, 12 };
static const int x64_win64_scratch_gpr[] = { 10, 11, 15 };
static const int x64_win64_scratch_fpr[] = { 13, 14, 15 };

static const anvil_x64_abi_desc_t x64_abi_descs[] = {
    {
        .abi = ANVIL_ABI_SYSV,
        .name = "sysv",
        .sym_prefix = "",
        .positional_args = false,
        .is_darwin = false,
        .is_win64 = false,
        .shadow_space = 0,
        .int_arg_regs = x64_sysv_int_args,
        .num_int_arg_regs = (int)(sizeof(x64_sysv_int_args) / sizeof(int)),
        .fp_arg_regs = x64_sysv_fp_args,
        .num_fp_arg_regs = (int)(sizeof(x64_sysv_fp_args) / sizeof(int)),
        .int_ret_reg = X64_RAX,
        .int_ret_hi_reg = X64_RDX,
        .fp_ret_reg = 0,
        .indirect_call_reg = X64_R11,
        .alloc_gpr_regs = x64_sysv_alloc_gpr,
        .num_alloc_gpr_regs = (int)(sizeof(x64_sysv_alloc_gpr) / sizeof(int)),
        .alloc_fpr_regs = NULL,
        .num_alloc_fpr_regs = 0,
        .scratch_gpr_regs = x64_sysv_scratch_gpr,
        .num_scratch_gpr_regs = (int)(sizeof(x64_sysv_scratch_gpr) / sizeof(int)),
        .scratch_fpr_regs = x64_sysv_scratch_fpr,
        .num_scratch_fpr_regs = (int)(sizeof(x64_sysv_scratch_fpr) / sizeof(int)),
    },
    {
        .abi = ANVIL_ABI_DARWIN,
        .name = "darwin",
        .sym_prefix = "_",
        .positional_args = false,
        .is_darwin = true,
        .is_win64 = false,
        .shadow_space = 0,
        .int_arg_regs = x64_sysv_int_args,
        .num_int_arg_regs = (int)(sizeof(x64_sysv_int_args) / sizeof(int)),
        .fp_arg_regs = x64_sysv_fp_args,
        .num_fp_arg_regs = (int)(sizeof(x64_sysv_fp_args) / sizeof(int)),
        .int_ret_reg = X64_RAX,
        .int_ret_hi_reg = X64_RDX,
        .fp_ret_reg = 0,
        .indirect_call_reg = X64_R11,
        .alloc_gpr_regs = x64_sysv_alloc_gpr,
        .num_alloc_gpr_regs = (int)(sizeof(x64_sysv_alloc_gpr) / sizeof(int)),
        .alloc_fpr_regs = NULL,
        .num_alloc_fpr_regs = 0,
        .scratch_gpr_regs = x64_sysv_scratch_gpr,
        .num_scratch_gpr_regs = (int)(sizeof(x64_sysv_scratch_gpr) / sizeof(int)),
        .scratch_fpr_regs = x64_sysv_scratch_fpr,
        .num_scratch_fpr_regs = (int)(sizeof(x64_sysv_scratch_fpr) / sizeof(int)),
    },
    {
        .abi = ANVIL_ABI_WIN64,
        .name = "win64",
        .sym_prefix = "",
        .positional_args = true,
        .is_darwin = false,
        .is_win64 = true,
        .shadow_space = 32,
        .int_arg_regs = x64_win64_int_args,
        .num_int_arg_regs = (int)(sizeof(x64_win64_int_args) / sizeof(int)),
        .fp_arg_regs = x64_win64_fp_args,
        .num_fp_arg_regs = (int)(sizeof(x64_win64_fp_args) / sizeof(int)),
        .int_ret_reg = X64_RAX,
        .int_ret_hi_reg = -1,
        .fp_ret_reg = 0,
        .indirect_call_reg = X64_R11,
        .alloc_gpr_regs = x64_win64_alloc_gpr,
        .num_alloc_gpr_regs = (int)(sizeof(x64_win64_alloc_gpr) / sizeof(int)),
        .alloc_fpr_regs = x64_win64_alloc_fpr,
        .num_alloc_fpr_regs = (int)(sizeof(x64_win64_alloc_fpr) / sizeof(int)),
        .scratch_gpr_regs = x64_win64_scratch_gpr,
        .num_scratch_gpr_regs = (int)(sizeof(x64_win64_scratch_gpr) / sizeof(int)),
        .scratch_fpr_regs = x64_win64_scratch_fpr,
        .num_scratch_fpr_regs = (int)(sizeof(x64_win64_scratch_fpr) / sizeof(int)),
    },
};

const anvil_x64_abi_desc_t *anvil_x64_get_abi_desc(anvil_abi_t abi)
{
    if (abi == ANVIL_ABI_DEFAULT) abi = ANVIL_ABI_SYSV;
    for (size_t i = 0; i < sizeof(x64_abi_descs) / sizeof(x64_abi_descs[0]); i++) {
        if (x64_abi_descs[i].abi == abi) return &x64_abi_descs[i];
    }
    return NULL;
}

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

typedef struct {
    anvil_mir_vreg_t dst;
    anvil_mir_vreg_t src;
} phi_edge_copy_t;

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
    return x64_type_is_fp(type) ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR;
}

static uint16_t x64_bits_for_type(anvil_type_t *type)
{
    if (!type) return 64;
    if (type->kind == ANVIL_TYPE_PTR) return 64;

    size_t size = anvil_type_size(type);
    if (size == 0) return 64;
    if (size > UINT16_MAX / 8) return 64;
    return (uint16_t)(size * 8);
}

static uint16_t x64_slot_bits_for_type(anvil_type_t *type)
{
    size_t size = type ? anvil_type_size(type) : 8;
    if (size == 0) size = 8;
    if (size > UINT16_MAX / 8) size = UINT16_MAX / 8;
    return (uint16_t)(size * 8);
}

static uint16_t x64_align_for_type(anvil_type_t *type)
{
    int align = type ? x64_type_align(type) : 8;
    if (align <= 0) align = 8;
    if (align > UINT16_MAX) align = UINT16_MAX;
    return (uint16_t)align;
}

static anvil_mir_vreg_t x64_add_vreg_for_type(x64_mir_lower_t *lower,
                                              anvil_type_t *type)
{
    return anvil_mir_add_vreg_typed(lower->mir,
                                    x64_reg_class_for_type(type),
                                    x64_bits_for_type(type),
                                    x64_type_is_signed(type));
}

static bool map_reserve(x64_mir_lower_t *lower, size_t needed)
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

static bool map_put(x64_mir_lower_t *lower, anvil_value_t *value,
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

static anvil_mir_vreg_t map_get(x64_mir_lower_t *lower, anvil_value_t *value)
{
    for (size_t i = 0; i < lower->num_values; i++) {
        if (lower->values[i].value == value) return lower->values[i].vreg;
    }
    return ANVIL_MIR_NO_VREG;
}

static bool addr_map_reserve(x64_mir_lower_t *lower, size_t needed)
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

static bool addr_map_put(x64_mir_lower_t *lower,
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

static bool addr_map_get(x64_mir_lower_t *lower,
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

static anvil_mir_block_t block_get(x64_mir_lower_t *lower,
                                   anvil_block_t *block)
{
    if (!block) return ANVIL_MIR_NO_BLOCK;
    for (size_t i = 0; i < lower->num_blocks; i++) {
        if (lower->blocks[i].block == block) return lower->blocks[i].mir_block;
    }
    return ANVIL_MIR_NO_BLOCK;
}

static bool create_mir_blocks(x64_mir_lower_t *lower)
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

static int x64_arg_reg_for(const anvil_x64_abi_desc_t *desc,
                           anvil_type_t *type,
                           size_t position,
                           size_t gpr_count,
                           size_t fpr_count)
{
    if (x64_type_is_fp(type)) {
        size_t idx = desc->positional_args ? position : fpr_count;
        if ((int)idx >= desc->num_fp_arg_regs) return -1;
        return desc->fp_arg_regs[idx];
    }
    size_t idx = desc->positional_args ? position : gpr_count;
    if ((int)idx >= desc->num_int_arg_regs) return -1;
    return desc->int_arg_regs[idx];
}

static bool arg_still_uses_register(const anvil_x64_abi_desc_t *desc,
                                    anvil_type_t *type,
                                    size_t position,
                                    size_t gpr_count,
                                    size_t fpr_count)
{
    return x64_arg_reg_for(desc, type, position, gpr_count, fpr_count) >= 0;
}

static void advance_arg_count(anvil_type_t *type,
                              size_t *gpr_count,
                              size_t *fpr_count)
{
    if (x64_type_is_fp(type)) {
        (*fpr_count)++;
    } else {
        (*gpr_count)++;
    }
}

static int64_t x64_stack_arg_slot_size(anvil_type_t *type)
{
    int64_t size = type ? x64_type_size(type) : 8;
    if (size <= 0) size = 8;
    if (size < 8) size = 8;
    return (size + 7) & ~INT64_C(7);
}

static bool lower_params(x64_mir_lower_t *lower)
{
    const anvil_x64_abi_desc_t *desc = lower->desc;
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

        anvil_mir_vreg_t local = x64_add_vreg_for_type(lower, param->type);
        if (local == ANVIL_MIR_NO_VREG) return false;

        if (arg_still_uses_register(desc, param->type, i,
                                    gpr_count, fpr_count)) {
            int reg = x64_arg_reg_for(desc, param->type, i,
                                      gpr_count, fpr_count);
            anvil_mir_vreg_t incoming = x64_add_vreg_for_type(lower, param->type);
            if (incoming == ANVIL_MIR_NO_VREG ||
                !anvil_mir_set_fixed_reg(lower->mir, incoming, reg)) {
                return false;
            }

            anvil_mir_vreg_t uses[] = { incoming };
            if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                                     local, uses, 1)) {
                return false;
            }
            advance_arg_count(param->type, &gpr_count, &fpr_count);
        } else {
            if (!anvil_mir_add_instr_imm(lower->mir,
                                         ANVIL_MIR_OP_INCOMING_STACK_ARG,
                                         local, stack_offset)) {
                return false;
            }
            stack_offset += x64_stack_arg_slot_size(param->type);
            advance_arg_count(param->type, &gpr_count, &fpr_count);
        }

        if (!map_put(lower, param, local)) return false;
    }

    return true;
}

static bool prepare_phi_results(x64_mir_lower_t *lower)
{
    for (anvil_block_t *block = lower->func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_PHI) break;
            if (!instr->result) return false;

            anvil_mir_vreg_t vreg = x64_add_vreg_for_type(lower,
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

static anvil_mir_vreg_t lower_value(x64_mir_lower_t *lower,
                                    anvil_value_t *value);
static bool lower_add_const_offset(x64_mir_lower_t *lower,
                                   anvil_mir_vreg_t base,
                                   int64_t offset,
                                   anvil_mir_vreg_t *out_ptr);

static anvil_mir_vreg_t lower_const_value(x64_mir_lower_t *lower,
                                          anvil_value_t *value)
{
    anvil_mir_vreg_t vreg = x64_add_vreg_for_type(lower, value->type);
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

static anvil_mir_vreg_t lower_symbol_address(x64_mir_lower_t *lower,
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

static anvil_mir_vreg_t lower_string_address(x64_mir_lower_t *lower,
                                             anvil_value_t *value)
{
    const char *label = NULL;
    if (anvil_mir_add_string_literal(lower->mir, value->data.str,
                                     &label) < 0 || !label) {
        return ANVIL_MIR_NO_VREG;
    }
    return lower_symbol_address(lower, label);
}

static anvil_mir_vreg_t lower_value(x64_mir_lower_t *lower,
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
            *out_op = ANVIL_MIR_OP_CMP;
            return true;
        default:
            return false;
    }
}

static bool mir_op_is_compare(anvil_mir_opcode_t op)
{
    switch (op) {
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

static bool add_return(x64_mir_lower_t *lower, anvil_value_t *value)
{
    if (!value) {
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET,
                                   ANVIL_MIR_NO_VREG, NULL, 0);
    }

    anvil_mir_vreg_t src = lower_value(lower, value);
    if (src == ANVIL_MIR_NO_VREG) return false;

    anvil_mir_vreg_t ret_uses[] = { src };
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

static bool emit_vreg_copy(anvil_mir_func_t *mir,
                           anvil_mir_vreg_t dst,
                           anvil_mir_vreg_t src)
{
    anvil_mir_vreg_t uses[] = { src };
    return anvil_mir_add_instr(mir, ANVIL_MIR_OP_COPY, dst, uses, 1);
}

static bool copy_dst_is_still_source(phi_edge_copy_t *copies,
                                     size_t num_copies,
                                     anvil_mir_vreg_t dst)
{
    for (size_t i = 0; i < num_copies; i++) {
        if (copies[i].src == dst) return true;
    }
    return false;
}

static void remove_phi_edge_copy(phi_edge_copy_t *copies,
                                 size_t *num_copies,
                                 size_t index)
{
    if (index >= *num_copies) return;

    for (size_t i = index + 1; i < *num_copies; i++) {
        copies[i - 1] = copies[i];
    }
    (*num_copies)--;
}

static bool break_parallel_copy_cycle(x64_mir_lower_t *lower,
                                      phi_edge_copy_t *copies,
                                      size_t num_copies)
{
    if (num_copies == 0) return true;

    anvil_mir_vreg_t saved = copies[0].dst;
    const anvil_mir_vreg_info_t *info =
        anvil_mir_get_vreg_info(lower->mir, saved);
    if (!info) return false;

    anvil_mir_vreg_t temp =
        anvil_mir_add_vreg_typed(lower->mir, info->reg_class,
                                 info->size_bits, info->is_signed);
    if (temp == ANVIL_MIR_NO_VREG) return false;

    if (!emit_vreg_copy(lower->mir, temp, saved)) return false;

    for (size_t i = 0; i < num_copies; i++) {
        if (copies[i].src == saved) {
            copies[i].src = temp;
        }
    }

    return true;
}

static bool emit_parallel_phi_edge_copies(x64_mir_lower_t *lower,
                                          phi_edge_copy_t *copies,
                                          size_t num_copies)
{
    while (num_copies > 0) {
        bool emitted = false;

        for (size_t i = 0; i < num_copies; i++) {
            if (copy_dst_is_still_source(copies, num_copies, copies[i].dst)) {
                continue;
            }

            if (!emit_vreg_copy(lower->mir, copies[i].dst, copies[i].src)) {
                return false;
            }
            remove_phi_edge_copy(copies, &num_copies, i);
            emitted = true;
            break;
        }

        if (emitted) continue;

        if (!break_parallel_copy_cycle(lower, copies, num_copies)) {
            return false;
        }
    }

    return true;
}

static bool lower_phi_copies_for_edge(x64_mir_lower_t *lower,
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

    bool ok = emit_parallel_phi_edge_copies(lower, copies, num_copies);
    free(copies);
    return ok;

fail:
    free(copies);
    return false;
}

static anvil_mir_block_t create_phi_edge_block(x64_mir_lower_t *lower,
                                               anvil_block_t *src_block,
                                               anvil_block_t *dest_block)
{
    const char *src_name = (src_block && src_block->name) ? src_block->name : "anon";
    const char *dest_name = (dest_block && dest_block->name) ? dest_block->name : "anon";

    char name[256];
    snprintf(name, sizeof(name), "%s_to_%s_phi_%zu",
             src_name, dest_name, lower->num_edge_blocks++);
    return anvil_mir_add_block(lower->mir, name);
}

static bool emit_phi_edge_block(x64_mir_lower_t *lower,
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

static bool prepare_phi_aware_target(x64_mir_lower_t *lower,
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

static bool emit_pending_phi_edges(x64_mir_lower_t *lower,
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

static anvil_mir_block_t create_switch_chain_block(x64_mir_lower_t *lower,
                                                   anvil_block_t *src_block,
                                                   size_t case_index)
{
    const char *src_name = (src_block && src_block->name) ? src_block->name : "anon";

    char name[256];
    snprintf(name, sizeof(name), "%s_switch_case_%zu_%zu",
             src_name, case_index, lower->num_edge_blocks++);
    return anvil_mir_add_block(lower->mir, name);
}

static int x64_call_num_vector_args(anvil_type_t *fn_type, anvil_instr_t *instr)
{
    int count = 0;
    for (size_t i = 1; i < instr->num_operands; i++) {
        anvil_value_t *arg = instr->operands[i];
        if (arg && x64_type_is_fp(arg->type)) count++;
    }
    (void)fn_type;
    return count;
}

static bool lower_call(x64_mir_lower_t *lower, anvil_instr_t *instr)
{
    const anvil_x64_abi_desc_t *desc = lower->desc;
    if (instr->num_operands == 0) return false;

    anvil_value_t *callee = instr->operands[0];
    anvil_type_t *fn_type = call_func_type(callee);
    if (!fn_type) return false;

    bool direct_call = call_is_direct_symbol(callee);
    const char *symbol = direct_call ? call_symbol(callee) : NULL;
    if (direct_call && !symbol) return false;

    bool is_variadic = fn_type->data.func.variadic;
    int vector_args = is_variadic ? x64_call_num_vector_args(fn_type, instr) : 0;

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
        anvil_mir_vreg_t target_fixed = x64_add_vreg_for_type(lower, callee->type);
        if (target_src == ANVIL_MIR_NO_VREG ||
            target_fixed == ANVIL_MIR_NO_VREG ||
            !anvil_mir_set_fixed_reg(lower->mir, target_fixed,
                                     desc->indirect_call_reg)) {
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

        if (!arg_still_uses_register(desc, arg->type, i,
                                     gpr_count, fpr_count)) {
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
            advance_arg_count(arg->type, &gpr_count, &fpr_count);
            continue;
        }

        int reg = x64_arg_reg_for(desc, arg->type, i, gpr_count, fpr_count);
        anvil_mir_vreg_t fixed_arg = x64_add_vreg_for_type(lower, arg->type);
        if (fixed_arg == ANVIL_MIR_NO_VREG ||
            !anvil_mir_set_fixed_reg(lower->mir, fixed_arg, reg)) {
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
        advance_arg_count(arg->type, &gpr_count, &fpr_count);
    }

    if (!ok) {
        free(call_uses);
        return false;
    }

    anvil_mir_vreg_t call_def = ANVIL_MIR_NO_VREG;
    if (instr->result && !x64_type_is_void(instr->result->type)) {
        call_def = x64_add_vreg_for_type(lower, instr->result->type);
        int ret_reg = x64_type_is_fp(instr->result->type) ? desc->fp_ret_reg
                                                           : desc->int_ret_reg;
        if (call_def == ANVIL_MIR_NO_VREG ||
            !anvil_mir_set_fixed_reg(lower->mir, call_def, ret_reg)) {
            free(call_uses);
            return false;
        }
    }

    (void)is_variadic;
    (void)vector_args;
    bool emit_ok = anvil_mir_add_instr_symbol(lower->mir, ANVIL_MIR_OP_CALL,
                                              call_def, call_uses,
                                              num_call_uses, symbol);
    free(call_uses);
    if (!emit_ok) return false;

    if (instr->result && call_def != ANVIL_MIR_NO_VREG) {
        anvil_mir_vreg_t local_result =
            x64_add_vreg_for_type(lower, instr->result->type);
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

static anvil_mir_vreg_t lower_widen_gpr_to_64(x64_mir_lower_t *lower,
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

static anvil_mir_vreg_t lower_resize_gpr(x64_mir_lower_t *lower,
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

static bool lower_match_binary_operand_sizes(x64_mir_lower_t *lower,
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

static bool lower_alloca(x64_mir_lower_t *lower, anvil_instr_t *instr)
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

    anvil_mir_vreg_t ptr = x64_add_vreg_for_type(lower, instr->result->type);
    if (ptr == ANVIL_MIR_NO_VREG) return false;

    if (instr->num_operands == 0) {
        int slot = anvil_mir_add_frame_slot(lower->mir,
                                            x64_slot_bits_for_type(element_type),
                                            x64_align_for_type(element_type));
        if (slot < 0) return false;
        if (!anvil_mir_add_frame_addr(lower->mir, ptr, slot)) return false;
        return map_put(lower, instr->result, ptr);
    }

    if (instr->num_operands != 1) return false;
    anvil_mir_vreg_t count = lower_value(lower, instr->operands[0]);
    if (count == ANVIL_MIR_NO_VREG) return false;
    count = lower_widen_gpr_to_64(lower, count, false);
    if (count == ANVIL_MIR_NO_VREG) return false;

    int64_t elem_size = x64_type_size(element_type);
    if (elem_size <= 0) elem_size = 1;
    anvil_mir_vreg_t uses[] = { count };
    if (!anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_DYN_ALLOCA,
                                      ptr, uses, 1, elem_size)) {
        return false;
    }
    return map_put(lower, instr->result, ptr);
}

static int64_t gep_element_size(anvil_instr_t *instr)
{
    anvil_type_t *element_type = NULL;
    if (instr && instr->result && instr->result->type &&
        instr->result->type->kind == ANVIL_TYPE_PTR) {
        element_type = instr->result->type->data.pointee;
    }

    int64_t elem_size = element_type ? x64_type_size(element_type) : 1;
    return elem_size > 0 ? elem_size : 1;
}

static bool lower_add_const_offset(x64_mir_lower_t *lower,
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

static bool lower_gep(x64_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands < 1 || !instr->result) return false;

    anvil_mir_vreg_t current = lower_value(lower, instr->operands[0]);
    if (current == ANVIL_MIR_NO_VREG) return false;
    current = lower_widen_gpr_to_64(lower, current, false);
    if (current == ANVIL_MIR_NO_VREG) return false;

    int64_t elem_size = gep_element_size(instr);
    bool all_constant = true;
    int64_t constant_offset = 0;
    for (size_t i = 1; i < instr->num_operands; i++) {
        anvil_value_t *index_value = instr->operands[i];
        if (!index_value || index_value->kind != ANVIL_VAL_CONST_INT) {
            all_constant = false;
            break;
        }
        constant_offset += index_value->data.i * elem_size;
    }
    if (all_constant) {
        return addr_map_put(lower, instr->result, current, constant_offset);
    }

    for (size_t i = 1; i < instr->num_operands; i++) {
        anvil_mir_vreg_t index = lower_value(lower, instr->operands[i]);
        if (index == ANVIL_MIR_NO_VREG) return false;
        index = lower_widen_gpr_to_64(lower, index, true);
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

    return map_put(lower, instr->result, current);
}

static bool lower_struct_gep(x64_mir_lower_t *lower, anvil_instr_t *instr)
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

static bool lower_cast(x64_mir_lower_t *lower,
                       anvil_instr_t *instr,
                       anvil_mir_opcode_t mir_op)
{
    if (instr->num_operands != 1 || !instr->result) return false;
    anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t def = x64_add_vreg_for_type(lower, instr->result->type);
    if (src == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) return false;

    anvil_mir_vreg_t uses[] = { src };
    if (!anvil_mir_add_instr(lower->mir, mir_op, def, uses, 1)) return false;
    return map_put(lower, instr->result, def);
}

static bool lower_select(x64_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands != 3 || !instr->result) return false;
    anvil_mir_vreg_t cond = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t then_v = lower_value(lower, instr->operands[1]);
    anvil_mir_vreg_t else_v = lower_value(lower, instr->operands[2]);
    anvil_mir_vreg_t def = x64_add_vreg_for_type(lower, instr->result->type);
    if (cond == ANVIL_MIR_NO_VREG ||
        then_v == ANVIL_MIR_NO_VREG ||
        else_v == ANVIL_MIR_NO_VREG ||
        def == ANVIL_MIR_NO_VREG) {
        return false;
    }

    anvil_mir_block_t source = anvil_mir_current_block(lower->mir);
    anvil_mir_block_t then_block =
        create_switch_chain_block(lower, instr->parent, 0);
    anvil_mir_block_t else_block =
        create_switch_chain_block(lower, instr->parent, 1);
    anvil_mir_block_t join_block =
        create_switch_chain_block(lower, instr->parent, 2);
    if (then_block == ANVIL_MIR_NO_BLOCK ||
        else_block == ANVIL_MIR_NO_BLOCK ||
        join_block == ANVIL_MIR_NO_BLOCK) {
        return false;
    }

    if (!anvil_mir_set_current_block(lower->mir, source) ||
        !anvil_mir_add_cond_branch(lower->mir, cond, then_block, else_block)) {
        return false;
    }

    if (!anvil_mir_set_current_block(lower->mir, then_block) ||
        !emit_vreg_copy(lower->mir, def, then_v) ||
        !anvil_mir_add_branch(lower->mir, join_block)) {
        return false;
    }

    if (!anvil_mir_set_current_block(lower->mir, else_block) ||
        !emit_vreg_copy(lower->mir, def, else_v) ||
        !anvil_mir_add_branch(lower->mir, join_block)) {
        return false;
    }

    if (!anvil_mir_set_current_block(lower->mir, join_block)) return false;
    return map_put(lower, instr->result, def);
}

static bool lower_memory_address(x64_mir_lower_t *lower,
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

static bool lower_switch(x64_mir_lower_t *lower, anvil_instr_t *instr)
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

static bool lower_instr(x64_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->op == ANVIL_OP_NOP) return true;
    if (instr->op == ANVIL_OP_PHI) return true;

    anvil_mir_opcode_t mir_op;
    if (instr->num_operands == 2 && map_binop(instr->op, &mir_op)) {
        anvil_mir_vreg_t lhs = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t rhs = lower_value(lower, instr->operands[1]);
        anvil_mir_vreg_t def = instr->result
            ? x64_add_vreg_for_type(lower, instr->result->type)
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
        if (!anvil_mir_add_instr(lower->mir, mir_op, def, uses, 2)) {
            return false;
        }
        return map_put(lower, instr->result, def);
    }

    if (instr->num_operands == 1 && map_unop(instr->op, &mir_op)) {
        anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t def = instr->result
            ? x64_add_vreg_for_type(lower, instr->result->type)
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
            anvil_mir_vreg_t def = x64_add_vreg_for_type(lower,
                                                         instr->result->type);
            if (ptr == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) return false;
            anvil_mir_vreg_t uses[] = { ptr };
            bool ok = offset == 0
                ? anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_LOAD, def,
                                      uses, 1)
                : anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_LOAD,
                                               def, uses, 1, offset);
            if (!ok) {
                return false;
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
            if (instr->num_operands == 0) return add_return(lower, NULL);
            if (instr->num_operands == 1) return add_return(lower, instr->operands[0]);
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
    if (func) {
        if (func->cc == ANVIL_CC_WIN64) abi = ANVIL_ABI_WIN64;
        else if (func->cc == ANVIL_CC_SYSV) abi = ANVIL_ABI_SYSV;
    }
    if (abi == ANVIL_ABI_DEFAULT) abi = ANVIL_ABI_SYSV;
    return abi;
}

anvil_mir_func_t *anvil_x86_64_lower_func_to_mir(anvil_func_t *func)
{
    if (!func || func->is_declaration) return NULL;

    const anvil_x64_abi_desc_t *desc = anvil_x64_get_abi_desc(x64_lower_abi(func));
    if (!desc) return NULL;

    x64_mir_lower_t lower;
    memset(&lower, 0, sizeof(lower));
    lower.desc = desc;
    lower.func = func;
    lower.mir = anvil_mir_func_create(func->name);
    if (!lower.mir) return NULL;

    if (!create_mir_blocks(&lower) ||
        !lower_params(&lower) ||
        !prepare_phi_results(&lower)) {
        anvil_mir_func_destroy(lower.mir);
        free(lower.blocks);
        free(lower.values);
        free(lower.addr_offsets);
        return NULL;
    }

    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        anvil_mir_block_t mir_block = block_get(&lower, block);
        if (mir_block == ANVIL_MIR_NO_BLOCK ||
            !anvil_mir_set_current_block(lower.mir, mir_block)) {
            anvil_mir_func_destroy(lower.mir);
            free(lower.blocks);
            free(lower.values);
            free(lower.addr_offsets);
            return NULL;
        }

        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (!lower_instr(&lower, instr)) {
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
    return lower.mir;
}

static bool x64_legal_fail(char *error, size_t error_len,
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

static bool x64_legal_size_for_class(const anvil_mir_vreg_info_t *info)
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

static bool x64_legal_fixed_reg(const anvil_mir_vreg_info_t *info)
{
    if (!info || !info->has_fixed_reg) return true;
    if (info->fixed_phys_reg < 0) return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR) return info->fixed_phys_reg <= 15;
    if (info->reg_class == ANVIL_MIR_REG_FPR) return info->fixed_phys_reg <= 15;
    return false;
}

static const anvil_mir_vreg_info_t *x64_legal_vreg_info(
    const anvil_mir_func_t *mir,
    anvil_mir_vreg_t vreg,
    size_t instr_index,
    char *error,
    size_t error_len)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, vreg);
    if (!info) {
        x64_legal_fail(error, error_len,
                       "x86-64 MIR instruction %zu uses invalid vreg",
                       instr_index);
        return NULL;
    }
    if (!x64_legal_size_for_class(info)) {
        x64_legal_fail(error, error_len,
                       "x86-64 MIR instruction %zu uses unsupported vreg class/size",
                       instr_index);
        return NULL;
    }
    if (!x64_legal_fixed_reg(info)) {
        x64_legal_fail(error, error_len,
                       "x86-64 MIR instruction %zu uses invalid fixed register",
                       instr_index);
        return NULL;
    }
    return info;
}

static bool x64_legal_pointer_operand(const anvil_mir_func_t *mir,
                                      anvil_mir_vreg_t vreg,
                                      size_t instr_index,
                                      char *error,
                                      size_t error_len)
{
    const anvil_mir_vreg_info_t *info =
        x64_legal_vreg_info(mir, vreg, instr_index, error, error_len);
    if (!info) return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR && info->size_bits == 64) {
        return true;
    }
    return x64_legal_fail(error, error_len,
                          "x86-64 MIR instruction %zu requires a 64-bit pointer operand",
                          instr_index);
}

static bool x64_legal_same_class_and_size(const anvil_mir_vreg_info_t *a,
                                          const anvil_mir_vreg_info_t *b)
{
    return a && b &&
           a->reg_class == b->reg_class &&
           a->size_bits == b->size_bits;
}

static bool x64_legal_binary(const anvil_mir_func_t *mir,
                             size_t instr_index,
                             const anvil_mir_instr_info_t *instr,
                             char *error,
                             size_t error_len)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(mir, instr_index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(mir, instr_index, 1);
    const anvil_mir_vreg_info_t *def =
        x64_legal_vreg_info(mir, instr->def, instr_index, error, error_len);
    const anvil_mir_vreg_info_t *lhs_info =
        x64_legal_vreg_info(mir, lhs, instr_index, error, error_len);
    const anvil_mir_vreg_info_t *rhs_info =
        x64_legal_vreg_info(mir, rhs, instr_index, error, error_len);
    if (!def || !lhs_info || !rhs_info) return false;

    if (!x64_legal_same_class_and_size(def, lhs_info) ||
        !x64_legal_same_class_and_size(lhs_info, rhs_info)) {
        return x64_legal_fail(error, error_len,
                              "x86-64 MIR instruction %zu has incompatible binary operands",
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

    return x64_legal_fail(error, error_len,
                          "x86-64 MIR instruction %zu uses an illegal binary opcode/class pair",
                          instr_index);
}

static bool x64_legal_call(const anvil_mir_func_t *mir,
                           size_t instr_index,
                           const anvil_mir_instr_info_t *instr,
                           char *error,
                           size_t error_len)
{
    if (instr->def != ANVIL_MIR_NO_VREG) {
        const anvil_mir_vreg_info_t *def =
            x64_legal_vreg_info(mir, instr->def, instr_index,
                                error, error_len);
        if (!def) return false;
        if (!def->has_fixed_reg) {
            return x64_legal_fail(error, error_len,
                                  "x86-64 MIR call %zu result must be fixed to ABI result register",
                                  instr_index);
        }
    }

    size_t arg_start = 0;
    if (!instr->symbol || !instr->symbol[0]) {
        if (instr->num_uses == 0) {
            return x64_legal_fail(error, error_len,
                                  "x86-64 MIR indirect call %zu requires a target register",
                                  instr_index);
        }

        anvil_mir_vreg_t target = anvil_mir_get_instr_use(mir, instr_index, 0);
        const anvil_mir_vreg_info_t *target_info =
            x64_legal_vreg_info(mir, target, instr_index, error, error_len);
        if (!target_info) return false;
        if (target_info->reg_class != ANVIL_MIR_REG_GPR ||
            target_info->size_bits != 64 ||
            !target_info->has_fixed_reg) {
            return x64_legal_fail(error, error_len,
                                  "x86-64 MIR indirect call %zu target must be a fixed pointer register",
                                  instr_index);
        }
        arg_start = 1;
    }

    for (size_t u = arg_start; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, instr_index, u);
        const anvil_mir_vreg_info_t *info =
            x64_legal_vreg_info(mir, use, instr_index, error, error_len);
        if (!info) return false;
        if (!info->has_fixed_reg) {
            return x64_legal_fail(error, error_len,
                                  "x86-64 MIR call %zu argument %zu must use a fixed ABI register",
                                  instr_index, u - arg_start);
        }
    }

    return true;
}

static bool x64_legal_instr(const anvil_mir_func_t *mir,
                            size_t instr_index,
                            const anvil_mir_instr_info_t *instr,
                            char *error,
                            size_t error_len)
{
    if (instr->def != ANVIL_MIR_NO_VREG &&
        !x64_legal_vreg_info(mir, instr->def, instr_index,
                             error, error_len)) {
        return false;
    }
    for (size_t u = 0; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, instr_index, u);
        if (!x64_legal_vreg_info(mir, use, instr_index,
                                 error, error_len)) {
            return false;
        }
    }

    switch (instr->op) {
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
            return x64_legal_binary(mir, instr_index, instr,
                                    error, error_len);

        case ANVIL_MIR_OP_LOAD: {
            anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(mir, instr_index, 0);
            return x64_legal_pointer_operand(mir, ptr, instr_index,
                                             error, error_len);
        }

        case ANVIL_MIR_OP_STORE: {
            anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(mir, instr_index, 1);
            return x64_legal_pointer_operand(mir, ptr, instr_index,
                                             error, error_len);
        }

        case ANVIL_MIR_OP_SYMBOL_ADDR:
        case ANVIL_MIR_OP_FRAME_ADDR:
        case ANVIL_MIR_OP_DYN_ALLOCA: {
            const anvil_mir_vreg_info_t *def =
                x64_legal_vreg_info(mir, instr->def, instr_index,
                                    error, error_len);
            if (def && def->reg_class == ANVIL_MIR_REG_GPR &&
                def->size_bits == 64) {
                return true;
            }
            return x64_legal_fail(error, error_len,
                                  "x86-64 MIR instruction %zu must define a 64-bit pointer",
                                  instr_index);
        }

        case ANVIL_MIR_OP_INCOMING_STACK_ARG:
        case ANVIL_MIR_OP_CALL_STACK_ARG:
            if (instr->has_imm && instr->imm >= 0 && (instr->imm % 8) == 0) {
                return true;
            }
            return x64_legal_fail(error, error_len,
                                  "x86-64 MIR stack instruction %zu needs an aligned stack offset",
                                  instr_index);

        case ANVIL_MIR_OP_CALL:
            return x64_legal_call(mir, instr_index, instr,
                                  error, error_len);

        case ANVIL_MIR_OP_SELECT:
            return x64_legal_fail(error, error_len,
                                  "x86-64 MIR select %zu must be lowered to a branch",
                                  instr_index);

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
        case ANVIL_MIR_OP_OTHER:
            return true;

        case ANVIL_MIR_OP_INVALID:
        default:
            break;
    }

    return x64_legal_fail(error, error_len,
                          "x86-64 MIR instruction %zu uses unsupported opcode",
                          instr_index);
}

bool anvil_x86_64_verify_mir_legal(const anvil_mir_func_t *mir,
                                   char *error,
                                   size_t error_len)
{
    if (error && error_len > 0) error[0] = '\0';
    if (!anvil_mir_verify(mir, error, error_len)) return false;

    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, i, &instr)) {
            return x64_legal_fail(error, error_len,
                                  "x86-64 MIR instruction %zu is not inspectable",
                                  i);
        }
        if (!x64_legal_instr(mir, i, &instr, error, error_len)) {
            return false;
        }
    }

    return true;
}

bool anvil_x86_64_regalloc_mir(anvil_mir_func_t *mir)
{
    if (!mir) return false;

    const anvil_x64_abi_desc_t *desc = anvil_x64_get_abi_desc(ANVIL_ABI_SYSV);
    if (!desc) return false;

    anvil_regalloc_class_config_t configs[2];
    size_t num_configs = 0;
    configs[num_configs].reg_class = ANVIL_MIR_REG_GPR;
    configs[num_configs].num_phys_regs = desc->num_alloc_gpr_regs;
    configs[num_configs].phys_regs = desc->alloc_gpr_regs;
    num_configs++;
    configs[num_configs].reg_class = ANVIL_MIR_REG_FPR;
    configs[num_configs].num_phys_regs = desc->num_alloc_fpr_regs;
    configs[num_configs].phys_regs = desc->alloc_fpr_regs;
    num_configs++;

    anvil_regalloc_class_config_t scratch_configs[2];
    size_t num_scratch_configs = 0;
    scratch_configs[num_scratch_configs].reg_class = ANVIL_MIR_REG_GPR;
    scratch_configs[num_scratch_configs].num_phys_regs = desc->num_scratch_gpr_regs;
    scratch_configs[num_scratch_configs].phys_regs = desc->scratch_gpr_regs;
    num_scratch_configs++;
    scratch_configs[num_scratch_configs].reg_class = ANVIL_MIR_REG_FPR;
    scratch_configs[num_scratch_configs].num_phys_regs = desc->num_scratch_fpr_regs;
    scratch_configs[num_scratch_configs].phys_regs = desc->scratch_fpr_regs;
    num_scratch_configs++;

    if (!anvil_x86_64_verify_mir_legal(mir, NULL, 0)) return false;
    if (!anvil_mir_coalesce_copies(mir)) return false;
    if (!anvil_x86_64_verify_mir_legal(mir, NULL, 0)) return false;
    if (!anvil_regalloc_linear_scan_classes(mir, configs, num_configs)) {
        return false;
    }
    if (!anvil_mir_materialize_spills(mir, scratch_configs,
                                      num_scratch_configs)) {
        return false;
    }
    return anvil_x86_64_verify_mir_legal(mir, NULL, 0);
}

typedef struct {
    const anvil_mir_func_t *mir;
    const anvil_x64_abi_desc_t *desc;
    anvil_syntax_t syntax;
    anvil_strbuf_t code;
    int *spill_offsets;
    size_t num_spill_offsets;
    int *frame_slot_offsets;
    size_t num_frame_slot_offsets;
    int gpr_save_offsets[16];
    int outgoing_size;
    int frame_size;
    bool has_frame;
    bool emitted_fneg_mask;
    bool emitted_fabs_mask;
    bool failed;
} x64_mir_emit_t;

static int x64_align_int(int value, int align)
{
    return (value + align - 1) & ~(align - 1);
}

static int x64_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0) return 8;
    int size = (int)((size_bits + 7) / 8);
    if (size <= 0) return 8;
    if (size > 8) return 8;
    return size;
}

static int x64_slot_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0) return 8;
    int size = (int)((size_bits + 7) / 8);
    return size > 0 ? size : 8;
}

static const anvil_mir_vreg_info_t *x64_vreg_info_checked(
    x64_mir_emit_t *emit,
    anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(emit->mir, vreg);
    if (!info) emit->failed = true;
    return info;
}

static const anvil_regalloc_assignment_t *x64_assignment_checked(
    x64_mir_emit_t *emit,
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

static const char *x64_gpr_name(int phys_reg, int size)
{
    if (phys_reg < 0 || phys_reg >= 16) return "?";
    switch (size) {
        case 1: return x64_reg8_names[phys_reg];
        case 2: return x64_reg16_names[phys_reg];
        case 4: return x64_reg32_names[phys_reg];
        default: return x64_reg64_names[phys_reg];
    }
}

static int x64_phys_of(x64_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *assignment =
        x64_assignment_checked(emit, vreg);
    return assignment ? assignment->phys_reg : -1;
}

static const char *x64_reg_name(x64_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = x64_vreg_info_checked(emit, vreg);
    const anvil_regalloc_assignment_t *assignment =
        x64_assignment_checked(emit, vreg);
    if (!info || !assignment) return "?";

    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        if (assignment->phys_reg < 0 || assignment->phys_reg >= 16) {
            emit->failed = true;
            return "?";
        }
        return x64_xmm_names[assignment->phys_reg];
    }

    int size = x64_size_bytes(info->size_bits);
    return x64_gpr_name(assignment->phys_reg, size);
}

static bool x64_emit_label(x64_mir_emit_t *emit, anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info)) return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s:\n",
                         anvil_mir_func_name(emit->mir), name);
    return true;
}

static bool x64_emit_branch_target(x64_mir_emit_t *emit,
                                   anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info)) return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s",
                         anvil_mir_func_name(emit->mir), name);
    return true;
}

static const char *x64_symbol_prefix(const x64_mir_emit_t *emit)
{
    return emit && emit->desc ? emit->desc->sym_prefix : "";
}

static bool x64_symbol_is_local(const char *symbol)
{
    return symbol && symbol[0] == '.';
}

static const char *x64_symbol_ref_prefix(const x64_mir_emit_t *emit,
                                         const char *symbol)
{
    return x64_symbol_is_local(symbol) ? "" : x64_symbol_prefix(emit);
}

static const char *x64_setcc(anvil_mir_opcode_t op)
{
    switch (op) {
        case ANVIL_MIR_OP_CMP_EQ:  return "sete";
        case ANVIL_MIR_OP_CMP_NE:
        case ANVIL_MIR_OP_CMP:     return "setne";
        case ANVIL_MIR_OP_CMP_LT:  return "setl";
        case ANVIL_MIR_OP_CMP_LE:  return "setle";
        case ANVIL_MIR_OP_CMP_GT:  return "setg";
        case ANVIL_MIR_OP_CMP_GE:  return "setge";
        case ANVIL_MIR_OP_CMP_ULT: return "setb";
        case ANVIL_MIR_OP_CMP_ULE: return "setbe";
        case ANVIL_MIR_OP_CMP_UGT: return "seta";
        case ANVIL_MIR_OP_CMP_UGE: return "setae";
        default: return "setne";
    }
}

static const char *x64_size_suffix(int size)
{
    switch (size) {
        case 1: return "b";
        case 2: return "w";
        case 4: return "l";
        default: return "q";
    }
}

static bool x64_instr_has_call(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info)) return true;
        if (info.op == ANVIL_MIR_OP_CALL) return true;
    }
    return false;
}

static bool x64_scan_outgoing_stack_args(x64_mir_emit_t *emit)
{
    int outgoing_size = 0;

    for (size_t i = 0; i < anvil_mir_num_instrs(emit->mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(emit->mir, i, &info)) return false;
        if (info.op != ANVIL_MIR_OP_CALL_STACK_ARG) continue;
        if (!info.has_imm || info.imm < 0 || info.num_uses != 1) return false;
        if (info.imm > INT32_MAX - 8) return false;

        int end = (int)info.imm + 8;
        if (end > outgoing_size) outgoing_size = end;
    }

    if (emit->desc->shadow_space > outgoing_size && x64_instr_has_call(emit->mir)) {
        outgoing_size = emit->desc->shadow_space;
    } else if (outgoing_size > 0) {
        outgoing_size += emit->desc->shadow_space;
    }
    emit->outgoing_size = x64_align_int(outgoing_size, 16);
    return true;
}

static bool x64_prepare_frame(x64_mir_emit_t *emit)
{
    for (size_t i = 0; i < 16; i++) {
        emit->gpr_save_offsets[i] = -1;
    }

    if (!x64_scan_outgoing_stack_args(emit)) return false;

    int offset = 0;
    static const int callee_saved[] = { X64_RBX, X64_R12, X64_R13, X64_R14, X64_R15 };
    for (size_t c = 0; c < sizeof(callee_saved) / sizeof(callee_saved[0]); c++) {
        int reg = callee_saved[c];
        bool used = false;
        for (size_t i = 0; i < anvil_mir_num_vregs(emit->mir); i++) {
            const anvil_regalloc_assignment_t *assignment =
                anvil_mir_get_assignment(emit->mir, (anvil_mir_vreg_t)i);
            if (!assignment || assignment->spilled) continue;
            if (assignment->reg_class == ANVIL_MIR_REG_GPR &&
                assignment->phys_reg == reg) {
                used = true;
                break;
            }
        }
        if (used) {
            offset += 8;
            emit->gpr_save_offsets[reg] = offset;
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
        offset = x64_align_int(offset, align);
        offset += x64_slot_size_bytes(slot.size_bits);
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
        offset += x64_align_int(x64_size_bytes(slot.size_bits), 8);
        emit->spill_offsets[i] = offset;
    }

    offset += emit->outgoing_size;
    emit->frame_size = x64_align_int(offset, 16);
    emit->has_frame = true;
    return true;
}

static void x64_emit_prologue(x64_mir_emit_t *emit)
{
    const char *name = anvil_mir_func_name(emit->mir);
    const char *prefix = x64_symbol_prefix(emit);
    if (emit->desc->is_darwin) {
        anvil_strbuf_append(&emit->code,
                            "\t.section __TEXT,__text,regular,pure_instructions\n");
        anvil_strbuf_appendf(&emit->code, "\t.globl %s%s\n", prefix, name);
        anvil_strbuf_append(&emit->code, "\t.p2align 4\n");
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    } else {
        anvil_strbuf_append(&emit->code, "\t.text\n");
        anvil_strbuf_appendf(&emit->code, "\t.globl %s%s\n", prefix, name);
        anvil_strbuf_appendf(&emit->code, "\t.type %s%s, @function\n",
                             prefix, name);
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    }

    anvil_strbuf_append(&emit->code, "\tpushq %rbp\n");
    anvil_strbuf_append(&emit->code, "\tmovq %rsp, %rbp\n");
    if (emit->frame_size > 0) {
        anvil_strbuf_appendf(&emit->code, "\tsubq $%d, %%rsp\n", emit->frame_size);
    }

    for (int reg = 0; reg < 16; reg++) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->code, "\tmovq %%%s, -%d(%%rbp)\n",
                                 x64_reg64_names[reg],
                                 emit->gpr_save_offsets[reg]);
        }
    }
}

static void x64_emit_epilogue(x64_mir_emit_t *emit)
{
    for (int reg = 15; reg >= 0; reg--) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->code, "\tmovq -%d(%%rbp), %%%s\n",
                                 emit->gpr_save_offsets[reg],
                                 x64_reg64_names[reg]);
        }
    }
    anvil_strbuf_append(&emit->code, "\tmovq %rbp, %rsp\n");
    anvil_strbuf_append(&emit->code, "\tpopq %rbp\n");
    anvil_strbuf_append(&emit->code, "\tret\n");
}

static const char *x64_fp_mov_op(int size)
{
    return size <= 4 ? "movss" : "movsd";
}

static void x64_emit_copy(x64_mir_emit_t *emit,
                          anvil_mir_vreg_t dst,
                          anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *dst_info = x64_vreg_info_checked(emit, dst);
    const anvil_mir_vreg_info_t *src_info = x64_vreg_info_checked(emit, src);
    int dst_phys = x64_phys_of(emit, dst);
    int src_phys = x64_phys_of(emit, src);
    if (!dst_info || !src_info || emit->failed) return;

    const char *dst_reg = x64_reg_name(emit, dst);
    const char *src_reg = x64_reg_name(emit, src);
    if (emit->failed) return;

    if (dst_info->reg_class == ANVIL_MIR_REG_FPR &&
        src_info->reg_class == ANVIL_MIR_REG_FPR) {
        if (dst_phys == src_phys) return;
        int size = x64_size_bytes(dst_info->size_bits);
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n",
                             x64_fp_mov_op(size), src_reg, dst_reg);
    } else if (dst_info->reg_class == ANVIL_MIR_REG_GPR &&
               src_info->reg_class == ANVIL_MIR_REG_GPR) {
        if (dst_phys == src_phys) return;
        int size = x64_size_bytes(dst_info->size_bits);
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n",
                             x64_size_suffix(size),
                             x64_gpr_name(src_phys, size),
                             x64_gpr_name(dst_phys, size));
    } else if (dst_info->reg_class == ANVIL_MIR_REG_FPR &&
               src_info->reg_class == ANVIL_MIR_REG_GPR) {
        anvil_strbuf_appendf(&emit->code, "\tmovq %%%s, %%%s\n",
                             x64_gpr_name(src_phys, 8), dst_reg);
    } else if (dst_info->reg_class == ANVIL_MIR_REG_GPR &&
               src_info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\tmovq %%%s, %%%s\n",
                             src_reg, x64_gpr_name(dst_phys, 8));
    } else {
        emit->failed = true;
    }
}

static void x64_emit_mov(x64_mir_emit_t *emit,
                         const anvil_mir_instr_info_t *info)
{
    const anvil_mir_vreg_info_t *def_info = x64_vreg_info_checked(emit, info->def);
    if (!def_info) return;
    int dst_phys = x64_phys_of(emit, info->def);
    if (emit->failed) return;

    int64_t imm = info->has_imm ? info->imm : 0;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x64_size_bytes(def_info->size_bits);
        const char *dst = x64_xmm_names[dst_phys];
        if (size <= 4) {
            anvil_strbuf_appendf(&emit->code, "\tmovl $%u, %%r11d\n",
                                 (uint32_t)imm);
            anvil_strbuf_appendf(&emit->code, "\tmovd %%r11d, %%%s\n", dst);
        } else {
            anvil_strbuf_appendf(&emit->code, "\tmovabsq $%llu, %%r11\n",
                                 (unsigned long long)(uint64_t)imm);
            anvil_strbuf_appendf(&emit->code, "\tmovq %%r11, %%%s\n", dst);
        }
        return;
    }

    int size = x64_size_bytes(def_info->size_bits);
    if (size < 8) {
        anvil_strbuf_appendf(&emit->code, "\tmovl $%lld, %%%s\n",
                             (long long)(int64_t)(int32_t)imm,
                             x64_gpr_name(dst_phys, 4));
    } else {
        anvil_strbuf_appendf(&emit->code, "\tmovabsq $%lld, %%%s\n",
                             (long long)imm, x64_gpr_name(dst_phys, 8));
    }
}

static void x64_emit_gpr_binary(x64_mir_emit_t *emit,
                                const anvil_mir_instr_info_t *info,
                                int a_phys, int b_phys, int size)
{
    const char *suf = x64_size_suffix(size);
    const char *rax = x64_gpr_name(X64_RAX, size);
    const char *rdx = x64_gpr_name(X64_RDX, size);
    const char *rcx = x64_gpr_name(X64_RCX, size);
    const char *a = x64_gpr_name(a_phys, size);
    const char *b = x64_gpr_name(b_phys, size);
    int dst_phys = x64_phys_of(emit, info->def);
    const char *dst = x64_gpr_name(dst_phys, size);
    if (emit->failed) return;

    switch (info->op) {
        case ANVIL_MIR_OP_ADD:
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
            anvil_strbuf_appendf(&emit->code, "\tadd%s %%%s, %%%s\n", suf, b, rax);
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, rax, dst);
            break;
        case ANVIL_MIR_OP_SUB:
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
            anvil_strbuf_appendf(&emit->code, "\tsub%s %%%s, %%%s\n", suf, b, rax);
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, rax, dst);
            break;
        case ANVIL_MIR_OP_MUL:
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
            anvil_strbuf_appendf(&emit->code, "\timul%s %%%s, %%%s\n", suf, b, rax);
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, rax, dst);
            break;
        case ANVIL_MIR_OP_AND:
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
            anvil_strbuf_appendf(&emit->code, "\tand%s %%%s, %%%s\n", suf, b, rax);
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, rax, dst);
            break;
        case ANVIL_MIR_OP_OR:
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
            anvil_strbuf_appendf(&emit->code, "\tor%s %%%s, %%%s\n", suf, b, rax);
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, rax, dst);
            break;
        case ANVIL_MIR_OP_XOR:
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
            anvil_strbuf_appendf(&emit->code, "\txor%s %%%s, %%%s\n", suf, b, rax);
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, rax, dst);
            break;
        case ANVIL_MIR_OP_SHL:
        case ANVIL_MIR_OP_SHR:
        case ANVIL_MIR_OP_SAR: {
            const char *sh = info->op == ANVIL_MIR_OP_SHL ? "shl"
                           : info->op == ANVIL_MIR_OP_SHR ? "shr" : "sar";
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, b, rcx);
            anvil_strbuf_appendf(&emit->code, "\t%s%s %%cl, %%%s\n", sh, suf, rax);
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, rax, dst);
            break;
        }
        case ANVIL_MIR_OP_SDIV:
        case ANVIL_MIR_OP_UDIV:
        case ANVIL_MIR_OP_SMOD:
        case ANVIL_MIR_OP_UMOD: {
            bool is_signed = info->op == ANVIL_MIR_OP_SDIV ||
                             info->op == ANVIL_MIR_OP_SMOD;
            bool is_mod = info->op == ANVIL_MIR_OP_SMOD ||
                          info->op == ANVIL_MIR_OP_UMOD;
            const char *r11 = x64_gpr_name(X64_R11, size);
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, b, r11);
            if (size == 1) {
                if (is_signed) {
                    anvil_strbuf_append(&emit->code, "\tcbtw\n");
                    anvil_strbuf_appendf(&emit->code, "\tidivb %%%s\n", r11);
                } else {
                    anvil_strbuf_append(&emit->code, "\tmovzbw %al, %ax\n");
                    anvil_strbuf_appendf(&emit->code, "\tdivb %%%s\n", r11);
                }
                if (is_mod) {
                    anvil_strbuf_append(&emit->code, "\tshrw $8, %ax\n");
                }
                anvil_strbuf_appendf(&emit->code, "\tmovb %%al, %%%s\n", dst);
            } else {
                if (is_signed) {
                    const char *ext = size == 2 ? "\tcwtd\n"
                                    : size == 4 ? "\tcltd\n" : "\tcqto\n";
                    anvil_strbuf_append(&emit->code, ext);
                    anvil_strbuf_appendf(&emit->code, "\tidiv%s %%%s\n", suf, r11);
                } else {
                    anvil_strbuf_appendf(&emit->code, "\txor%s %%%s, %%%s\n",
                                         suf, rdx, rdx);
                    anvil_strbuf_appendf(&emit->code, "\tdiv%s %%%s\n", suf, r11);
                }
                anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n",
                                     suf, is_mod ? rdx : rax, dst);
            }
            break;
        }
        default:
            emit->failed = true;
            break;
    }
}

static void x64_emit_binary(x64_mir_emit_t *emit,
                            size_t instr_index,
                            const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x64_vreg_info_checked(emit, info->def);
    if (!def_info) return;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x64_size_bytes(def_info->size_bits);
        const char *dst = x64_reg_name(emit, info->def);
        const char *a = x64_reg_name(emit, lhs);
        const char *b = x64_reg_name(emit, rhs);
        if (emit->failed) return;
        const char *op = NULL;
        const char *sfx = size <= 4 ? "ss" : "sd";
        switch (info->op) {
            case ANVIL_MIR_OP_ADD: op = "add"; break;
            case ANVIL_MIR_OP_SUB: op = "sub"; break;
            case ANVIL_MIR_OP_MUL: op = "mul"; break;
            case ANVIL_MIR_OP_DIV:
            case ANVIL_MIR_OP_FDIV: op = "div"; break;
            default: break;
        }
        if (!op) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n",
                             x64_fp_mov_op(size), a, dst);
        anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n",
                             op, sfx, b, dst);
        return;
    }

    int size = x64_size_bytes(def_info->size_bits);
    int a_phys = x64_phys_of(emit, lhs);
    int b_phys = x64_phys_of(emit, rhs);
    if (emit->failed) return;
    x64_emit_gpr_binary(emit, info, a_phys, b_phys, size);
}

static void x64_emit_cmp(x64_mir_emit_t *emit,
                         size_t instr_index,
                         const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *lhs_info = x64_vreg_info_checked(emit, lhs);
    int dst_phys = x64_phys_of(emit, info->def);
    if (!lhs_info || emit->failed) return;

    const char *dst8 = x64_gpr_name(dst_phys, 1);
    const char *dst32 = x64_gpr_name(dst_phys, 4);

    if (lhs_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x64_size_bytes(lhs_info->size_bits);
        const char *a = x64_reg_name(emit, lhs);
        const char *b = x64_reg_name(emit, rhs);
        if (emit->failed) return;
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n",
                             size <= 4 ? "ucomiss" : "ucomisd", b, a);
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s\n",
                             x64_setcc(info->op), dst8);
        anvil_strbuf_appendf(&emit->code, "\tmovzbl %%%s, %%%s\n", dst8, dst32);
        return;
    }

    int size = x64_size_bytes(lhs_info->size_bits);
    const char *suf = x64_size_suffix(size);
    const char *a = x64_gpr_name(x64_phys_of(emit, lhs), size);
    const char *b = x64_gpr_name(x64_phys_of(emit, rhs), size);
    if (emit->failed) return;
    anvil_strbuf_appendf(&emit->code, "\tcmp%s %%%s, %%%s\n", suf, b, a);
    anvil_strbuf_appendf(&emit->code, "\t%s %%%s\n", x64_setcc(info->op), dst8);
    anvil_strbuf_appendf(&emit->code, "\tmovzbl %%%s, %%%s\n", dst8, dst32);
}

static void x64_emit_unary(x64_mir_emit_t *emit,
                           size_t instr_index,
                           const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x64_vreg_info_checked(emit, info->def);
    if (!def_info || emit->failed) return;

    if (info->op == ANVIL_MIR_OP_NEG) {
        if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
            int size = x64_size_bytes(def_info->size_bits);
            const char *dst = x64_reg_name(emit, info->def);
            const char *s = x64_reg_name(emit, src);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n",
                                 x64_fp_mov_op(size), s, dst);
            anvil_strbuf_appendf(&emit->code, "\t%s .Lx64_fneg_mask%s(%%rip), %%%s\n",
                                 size <= 4 ? "xorps" : "xorpd",
                                 size <= 4 ? "32" : "64", dst);
            emit->emitted_fneg_mask = true;
        } else {
            int size = x64_size_bytes(def_info->size_bits);
            const char *suf = x64_size_suffix(size);
            const char *dst = x64_gpr_name(x64_phys_of(emit, info->def), size);
            const char *s = x64_gpr_name(x64_phys_of(emit, src), size);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, s, dst);
            anvil_strbuf_appendf(&emit->code, "\tneg%s %%%s\n", suf, dst);
        }
    } else if (info->op == ANVIL_MIR_OP_FABS &&
               def_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x64_size_bytes(def_info->size_bits);
        const char *dst = x64_reg_name(emit, info->def);
        const char *s = x64_reg_name(emit, src);
        if (emit->failed) return;
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n",
                             x64_fp_mov_op(size), s, dst);
        anvil_strbuf_appendf(&emit->code, "\t%s .Lx64_fabs_mask%s(%%rip), %%%s\n",
                             size <= 4 ? "andps" : "andpd",
                             size <= 4 ? "32" : "64", dst);
        emit->emitted_fabs_mask = true;
    } else if (info->op == ANVIL_MIR_OP_NOT &&
               def_info->reg_class == ANVIL_MIR_REG_GPR) {
        int size = x64_size_bytes(def_info->size_bits);
        const char *suf = x64_size_suffix(size);
        const char *dst = x64_gpr_name(x64_phys_of(emit, info->def), size);
        const char *s = x64_gpr_name(x64_phys_of(emit, src), size);
        if (emit->failed) return;
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, s, dst);
        anvil_strbuf_appendf(&emit->code, "\tnot%s %%%s\n", suf, dst);
    } else {
        emit->failed = true;
    }
}

static void x64_emit_gpr_extend(x64_mir_emit_t *emit,
                                size_t instr_index,
                                const anvil_mir_instr_info_t *info,
                                bool sign_extend)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = x64_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = x64_vreg_info_checked(emit, src);
    int dst_phys = x64_phys_of(emit, info->def);
    int src_phys = x64_phys_of(emit, src);
    if (!dst_info || !src_info || emit->failed) return;
    if (dst_info->reg_class != ANVIL_MIR_REG_GPR ||
        src_info->reg_class != ANVIL_MIR_REG_GPR) {
        emit->failed = true;
        return;
    }

    int src_size = x64_size_bytes(src_info->size_bits);
    int dst_size = x64_size_bytes(dst_info->size_bits);
    if (dst_size < src_size) dst_size = src_size;

    if (sign_extend) {
        if (src_size == dst_size) {
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n",
                                 x64_size_suffix(dst_size),
                                 x64_gpr_name(src_phys, dst_size),
                                 x64_gpr_name(dst_phys, dst_size));
            return;
        }
        const char *suffix = src_size == 1 ? (dst_size <= 4 ? "bl" : "bq")
                          : src_size == 2 ? (dst_size <= 4 ? "wl" : "wq")
                          : "lq";
        anvil_strbuf_appendf(&emit->code, "\tmovs%s %%%s, %%%s\n",
                             suffix,
                             x64_gpr_name(src_phys, src_size),
                             x64_gpr_name(dst_phys, dst_size));
        return;
    }

    if (src_size >= 4) {
        anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n",
                             x64_gpr_name(src_phys, 4),
                             x64_gpr_name(dst_phys, 4));
        return;
    }
    const char *suffix = src_size == 1 ? "bl" : "wl";
    anvil_strbuf_appendf(&emit->code, "\tmovz%s %%%s, %%%s\n",
                         suffix,
                         x64_gpr_name(src_phys, src_size),
                         x64_gpr_name(dst_phys, 4));
}

static void x64_emit_cast(x64_mir_emit_t *emit,
                          size_t instr_index,
                          const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = x64_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = x64_vreg_info_checked(emit, src);
    if (!dst_info || !src_info) return;

    switch (info->op) {
        case ANVIL_MIR_OP_ZEXT:
            x64_emit_gpr_extend(emit, instr_index, info, false);
            break;
        case ANVIL_MIR_OP_SEXT:
            x64_emit_gpr_extend(emit, instr_index, info, true);
            break;
        case ANVIL_MIR_OP_TRUNC:
        case ANVIL_MIR_OP_BITCAST:
            x64_emit_copy(emit, info->def, src);
            break;
        case ANVIL_MIR_OP_SITOFP: {
            if (dst_info->reg_class != ANVIL_MIR_REG_FPR ||
                src_info->reg_class != ANVIL_MIR_REG_GPR) {
                emit->failed = true;
                return;
            }
            int dst_size = x64_size_bytes(dst_info->size_bits);
            int src_size = x64_size_bytes(src_info->size_bits);
            if (src_size < 4) src_size = 4;
            const char *dst = x64_reg_name(emit, info->def);
            const char *s = x64_gpr_name(x64_phys_of(emit, src), src_size);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\tcvtsi2s%s%s %%%s, %%%s\n",
                                 dst_size <= 4 ? "s" : "d",
                                 src_size <= 4 ? "l" : "q", s, dst);
            break;
        }
        case ANVIL_MIR_OP_UITOFP: {
            if (dst_info->reg_class != ANVIL_MIR_REG_FPR ||
                src_info->reg_class != ANVIL_MIR_REG_GPR) {
                emit->failed = true;
                return;
            }
            int dst_size = x64_size_bytes(dst_info->size_bits);
            int src_size = x64_size_bytes(src_info->size_bits);
            const char *dst = x64_reg_name(emit, info->def);
            if (emit->failed) return;
            if (src_size <= 4) {
                const char *s = x64_gpr_name(x64_phys_of(emit, src), 4);
                anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%r11d\n", s);
                anvil_strbuf_appendf(&emit->code, "\tcvtsi2s%sq %%r11, %%%s\n",
                                     dst_size <= 4 ? "s" : "d", dst);
            } else {
                const char *s = x64_gpr_name(x64_phys_of(emit, src), 8);
                anvil_strbuf_appendf(&emit->code, "\tcvtsi2s%sq %%%s, %%%s\n",
                                     dst_size <= 4 ? "s" : "d", s, dst);
            }
            break;
        }
        case ANVIL_MIR_OP_FPTOSI:
        case ANVIL_MIR_OP_FPTOUI: {
            if (dst_info->reg_class != ANVIL_MIR_REG_GPR ||
                src_info->reg_class != ANVIL_MIR_REG_FPR) {
                emit->failed = true;
                return;
            }
            int src_size = x64_size_bytes(src_info->size_bits);
            int dst_size = x64_size_bytes(dst_info->size_bits);
            if (dst_size < 4) dst_size = 4;
            const char *s = x64_reg_name(emit, src);
            const char *dst = x64_gpr_name(x64_phys_of(emit, info->def),
                                           dst_size <= 4 ? 4 : 8);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\tcvtts%s2si %%%s, %%%s\n",
                                 src_size <= 4 ? "s" : "d", s, dst);
            break;
        }
        case ANVIL_MIR_OP_FPEXT: {
            const char *dst = x64_reg_name(emit, info->def);
            const char *s = x64_reg_name(emit, src);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\tcvtss2sd %%%s, %%%s\n", s, dst);
            break;
        }
        case ANVIL_MIR_OP_FPTRUNC: {
            const char *dst = x64_reg_name(emit, info->def);
            const char *s = x64_reg_name(emit, src);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\tcvtsd2ss %%%s, %%%s\n", s, dst);
            break;
        }
        default:
            emit->failed = true;
            break;
    }
}

static void x64_emit_frame_addr(x64_mir_emit_t *emit,
                                const anvil_mir_instr_info_t *info)
{
    if (info->frame_slot < 0 ||
        (size_t)info->frame_slot >= emit->num_frame_slot_offsets) {
        emit->failed = true;
        return;
    }

    int dst_phys = x64_phys_of(emit, info->def);
    if (emit->failed) return;

    int offset = emit->frame_slot_offsets[info->frame_slot];
    anvil_strbuf_appendf(&emit->code, "\tleaq -%d(%%rbp), %%%s\n",
                         offset, x64_gpr_name(dst_phys, 8));
}

static void x64_emit_dyn_alloca(x64_mir_emit_t *emit,
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

    const char *count_reg = x64_gpr_name(x64_phys_of(emit, count), 8);
    int dst_phys = x64_phys_of(emit, info->def);
    if (emit->failed) return;

    anvil_strbuf_appendf(&emit->code, "\tmovq %%%s, %%r11\n", count_reg);
    if (info->imm != 1) {
        anvil_strbuf_appendf(&emit->code, "\timulq $%lld, %%r11, %%r11\n",
                             (long long)info->imm);
    }
    anvil_strbuf_append(&emit->code, "\taddq $15, %r11\n");
    anvil_strbuf_append(&emit->code, "\tandq $-16, %r11\n");
    anvil_strbuf_append(&emit->code, "\tsubq %r11, %rsp\n");
    anvil_strbuf_appendf(&emit->code, "\tmovq %%rsp, %%%s\n",
                         x64_gpr_name(dst_phys, 8));
}

static void x64_emit_symbol_addr(x64_mir_emit_t *emit,
                                 const anvil_mir_instr_info_t *info)
{
    if (!info->symbol || !info->symbol[0]) {
        emit->failed = true;
        return;
    }

    int dst_phys = x64_phys_of(emit, info->def);
    if (emit->failed) return;

    const char *prefix = x64_symbol_ref_prefix(emit, info->symbol);
    const char *dst = x64_gpr_name(dst_phys, 8);
    if (x64_symbol_is_local(info->symbol)) {
        anvil_strbuf_appendf(&emit->code, "\tleaq %s%s(%%rip), %%%s\n",
                             prefix, info->symbol, dst);
    } else {
        anvil_strbuf_appendf(&emit->code, "\tleaq %s%s(%%rip), %%%s\n",
                             prefix, info->symbol, dst);
    }
}

static const char *x64_mem_load_op(anvil_mir_reg_class_t reg_class,
                                   int size, bool is_signed, int dst_size)
{
    if (reg_class == ANVIL_MIR_REG_FPR) {
        return size <= 4 ? "movss" : "movsd";
    }
    if (is_signed) {
        switch (size) {
            case 1: return dst_size <= 4 ? "movsbl" : "movsbq";
            case 2: return dst_size <= 4 ? "movswl" : "movswq";
            case 4: return dst_size <= 4 ? "movl" : "movslq";
            default: return "movq";
        }
    }
    switch (size) {
        case 1: return "movzbl";
        case 2: return "movzwl";
        case 4: return "movl";
        default: return "movq";
    }
}

static const char *x64_mem_store_op(anvil_mir_reg_class_t reg_class, int size)
{
    if (reg_class == ANVIL_MIR_REG_FPR) {
        return size <= 4 ? "movss" : "movsd";
    }
    switch (size) {
        case 1: return "movb";
        case 2: return "movw";
        case 4: return "movl";
        default: return "movq";
    }
}

static void x64_emit_load(x64_mir_emit_t *emit,
                          size_t instr_index,
                          const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    const anvil_mir_vreg_info_t *def_info = x64_vreg_info_checked(emit, info->def);
    int def_phys = x64_phys_of(emit, info->def);
    const char *base = x64_gpr_name(x64_phys_of(emit, ptr), 8);
    if (!def_info || emit->failed) return;

    int size = x64_size_bytes(def_info->size_bits);
    int64_t offset = info->has_imm ? info->imm : 0;
    const char *op = x64_mem_load_op(def_info->reg_class, size,
                                     def_info->is_signed, size);
    const char *dst;
    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        dst = x64_xmm_names[def_phys];
    } else if (!def_info->is_signed && size < 4) {
        dst = x64_gpr_name(def_phys, 4);
    } else if (def_info->is_signed && size <= 2) {
        dst = x64_gpr_name(def_phys, 4);
    } else {
        dst = x64_gpr_name(def_phys, size);
    }

    if (offset == 0) {
        anvil_strbuf_appendf(&emit->code, "\t%s (%%%s), %%%s\n", op, base, dst);
    } else {
        anvil_strbuf_appendf(&emit->code, "\t%s %lld(%%%s), %%%s\n",
                             op, (long long)offset, base, dst);
    }
}

static void x64_emit_store(x64_mir_emit_t *emit, size_t instr_index)
{
    anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (value == ANVIL_MIR_NO_VREG || ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    const anvil_mir_vreg_info_t *value_info = x64_vreg_info_checked(emit, value);
    const char *base = x64_gpr_name(x64_phys_of(emit, ptr), 8);
    if (!value_info || emit->failed) return;

    int size = x64_size_bytes(value_info->size_bits);
    const char *op = x64_mem_store_op(value_info->reg_class, size);
    const char *src;
    if (value_info->reg_class == ANVIL_MIR_REG_FPR) {
        src = x64_reg_name(emit, value);
    } else {
        src = x64_gpr_name(x64_phys_of(emit, value), size);
    }
    if (emit->failed) return;

    int64_t offset = 0;
    anvil_mir_instr_info_t full;
    if (anvil_mir_get_instr_info(emit->mir, instr_index, &full) && full.has_imm) {
        offset = full.imm;
    }
    if (offset == 0) {
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, (%%%s)\n", op, src, base);
    } else {
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %lld(%%%s)\n",
                             op, src, (long long)offset, base);
    }
}

static void x64_emit_incoming_stack_arg(x64_mir_emit_t *emit,
                                        const anvil_mir_instr_info_t *info)
{
    if (!info->has_imm || info->imm < 0 || info->imm > INT32_MAX - 32) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x64_vreg_info_checked(emit, info->def);
    int def_phys = x64_phys_of(emit, info->def);
    if (!def_info || emit->failed) return;

    int size = x64_size_bytes(def_info->size_bits);
    int shadow = emit->desc->shadow_space;
    int frame_offset = 16 + shadow + (int)info->imm;
    const char *op = x64_mem_load_op(def_info->reg_class, size,
                                     def_info->is_signed, size);
    const char *dst;
    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        dst = x64_xmm_names[def_phys];
    } else if (!def_info->is_signed && size < 4) {
        dst = x64_gpr_name(def_phys, 4);
    } else if (def_info->is_signed && size <= 2) {
        dst = x64_gpr_name(def_phys, 4);
    } else {
        dst = x64_gpr_name(def_phys, size);
    }
    anvil_strbuf_appendf(&emit->code, "\t%s %d(%%rbp), %%%s\n",
                         op, frame_offset, dst);
}

static void x64_emit_call_stack_arg(x64_mir_emit_t *emit,
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

    const anvil_mir_vreg_info_t *value_info = x64_vreg_info_checked(emit, value);
    if (!value_info || emit->failed) return;

    int size = x64_size_bytes(value_info->size_bits);
    const char *op = x64_mem_store_op(value_info->reg_class, size);
    const char *src;
    if (value_info->reg_class == ANVIL_MIR_REG_FPR) {
        src = x64_reg_name(emit, value);
    } else {
        src = x64_gpr_name(x64_phys_of(emit, value), size);
    }
    if (emit->failed) return;
    int slot = emit->desc->shadow_space + (int)info->imm;
    anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %d(%%rsp)\n", op, src, slot);
}

static int x64_count_xmm_args(x64_mir_emit_t *emit, size_t instr_index,
                              size_t arg_start, size_t num_uses)
{
    int count = 0;
    for (size_t u = arg_start; u < num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(emit->mir, instr_index, u);
        const anvil_mir_vreg_info_t *info =
            anvil_mir_get_vreg_info(emit->mir, use);
        if (info && info->reg_class == ANVIL_MIR_REG_FPR) count++;
    }
    return count;
}

static void x64_emit_call(x64_mir_emit_t *emit,
                          size_t instr_index,
                          const anvil_mir_instr_info_t *info)
{
    bool direct = info->symbol && info->symbol[0];
    size_t arg_start = direct ? 0 : 1;
    int xmm_args = x64_count_xmm_args(emit, instr_index, arg_start,
                                      info->num_uses);
    if (!emit->desc->is_win64) {
        anvil_strbuf_appendf(&emit->code, "\tmovb $%d, %%al\n", xmm_args);
    }

    if (direct) {
        anvil_strbuf_appendf(&emit->code, "\tcall %s%s\n",
                             x64_symbol_ref_prefix(emit, info->symbol),
                             info->symbol);
        return;
    }

    anvil_mir_vreg_t target = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (target == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    const char *target_reg = x64_gpr_name(x64_phys_of(emit, target), 8);
    if (emit->failed) return;
    anvil_strbuf_appendf(&emit->code, "\tcall *%%%s\n", target_reg);
}

static void x64_emit_spill_load(x64_mir_emit_t *emit,
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

    const anvil_mir_vreg_info_t *def_info = x64_vreg_info_checked(emit, info->def);
    int def_phys = x64_phys_of(emit, info->def);
    if (!def_info || emit->failed) return;

    int offset = emit->spill_offsets[info->spill_slot];
    int size = x64_size_bytes(slot.size_bits);
    if (slot.reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\t%s -%d(%%rbp), %%%s\n",
                             x64_fp_mov_op(size), offset,
                             x64_xmm_names[def_phys]);
    } else {
        anvil_strbuf_appendf(&emit->code, "\tmov%s -%d(%%rbp), %%%s\n",
                             x64_size_suffix(size), offset,
                             x64_gpr_name(def_phys, size));
    }
}

static void x64_emit_spill_store(x64_mir_emit_t *emit,
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

    int src_phys = x64_phys_of(emit, src_vreg);
    if (emit->failed) return;

    int offset = emit->spill_offsets[info->spill_slot];
    int size = x64_size_bytes(slot.size_bits);
    if (slot.reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, -%d(%%rbp)\n",
                             x64_fp_mov_op(size),
                             x64_xmm_names[src_phys], offset);
    } else {
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, -%d(%%rbp)\n",
                             x64_size_suffix(size),
                             x64_gpr_name(src_phys, size), offset);
    }
}

static void x64_emit_ret(x64_mir_emit_t *emit,
                         size_t instr_index,
                         const anvil_mir_instr_info_t *info)
{
    if (info->num_uses > 0) {
        anvil_mir_vreg_t ret = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        const anvil_mir_vreg_info_t *ret_info = x64_vreg_info_checked(emit, ret);
        int ret_phys = x64_phys_of(emit, ret);
        if (!ret_info || emit->failed) return;

        if (ret_info->reg_class == ANVIL_MIR_REG_FPR) {
            int size = x64_size_bytes(ret_info->size_bits);
            if (ret_phys != emit->desc->fp_ret_reg) {
                anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n",
                                     x64_fp_mov_op(size),
                                     x64_xmm_names[ret_phys],
                                     x64_xmm_names[emit->desc->fp_ret_reg]);
            }
        } else {
            int size = x64_size_bytes(ret_info->size_bits);
            if (size < 4) size = 4;
            if (ret_phys != emit->desc->int_ret_reg) {
                anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n",
                                     x64_size_suffix(size),
                                     x64_gpr_name(ret_phys, size),
                                     x64_gpr_name(emit->desc->int_ret_reg, size));
            }
        }
    }
    if (!emit->failed) x64_emit_epilogue(emit);
}

static void x64_emit_instr(x64_mir_emit_t *emit,
                           size_t instr_index,
                           const anvil_mir_instr_info_t *info)
{
    switch (info->op) {
        case ANVIL_MIR_OP_MOV:
            x64_emit_mov(emit, info);
            break;
        case ANVIL_MIR_OP_COPY: {
            anvil_mir_vreg_t src =
                anvil_mir_get_instr_use(emit->mir, instr_index, 0);
            if (src == ANVIL_MIR_NO_VREG) {
                emit->failed = true;
                break;
            }
            x64_emit_copy(emit, info->def, src);
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
            x64_emit_binary(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_NEG:
        case ANVIL_MIR_OP_NOT:
        case ANVIL_MIR_OP_FABS:
            x64_emit_unary(emit, instr_index, info);
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
            x64_emit_cast(emit, instr_index, info);
            break;
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
            x64_emit_cmp(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_SYMBOL_ADDR:
            x64_emit_symbol_addr(emit, info);
            break;
        case ANVIL_MIR_OP_LOAD:
            x64_emit_load(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_STORE:
            x64_emit_store(emit, instr_index);
            break;
        case ANVIL_MIR_OP_FRAME_ADDR:
            x64_emit_frame_addr(emit, info);
            break;
        case ANVIL_MIR_OP_DYN_ALLOCA:
            x64_emit_dyn_alloca(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_INCOMING_STACK_ARG:
            x64_emit_incoming_stack_arg(emit, info);
            break;
        case ANVIL_MIR_OP_CALL:
            x64_emit_call(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_CALL_STACK_ARG:
            x64_emit_call_stack_arg(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_BR:
            anvil_strbuf_append(&emit->code, "\tjmp ");
            if (!x64_emit_branch_target(emit, info->true_block)) {
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
            const anvil_mir_vreg_info_t *cond_info =
                x64_vreg_info_checked(emit, cond);
            if (!cond_info) break;
            int size = x64_size_bytes(cond_info->size_bits);
            const char *cond_reg = x64_gpr_name(x64_phys_of(emit, cond), size);
            if (emit->failed) break;
            anvil_strbuf_appendf(&emit->code, "\ttest%s %%%s, %%%s\n",
                                 x64_size_suffix(size), cond_reg, cond_reg);
            anvil_strbuf_append(&emit->code, "\tjne ");
            if (!x64_emit_branch_target(emit, info->true_block)) {
                emit->failed = true;
                break;
            }
            anvil_strbuf_append(&emit->code, "\n\tjmp ");
            if (!x64_emit_branch_target(emit, info->false_block)) {
                emit->failed = true;
                break;
            }
            anvil_strbuf_append(&emit->code, "\n");
            break;
        }
        case ANVIL_MIR_OP_RET:
            x64_emit_ret(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_SPILL_LOAD:
            x64_emit_spill_load(emit, info);
            break;
        case ANVIL_MIR_OP_SPILL_STORE:
            x64_emit_spill_store(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_OTHER:
            break;
        default:
            emit->failed = true;
            break;
    }
}

static void x64_emit_escaped_string(anvil_strbuf_t *code, const char *value)
{
    anvil_strbuf_append(code, "\t.asciz \"");
    for (const char *p = value ? value : ""; *p; p++) {
        switch (*p) {
            case '\n': anvil_strbuf_append(code, "\\n"); break;
            case '\r': anvil_strbuf_append(code, "\\r"); break;
            case '\t': anvil_strbuf_append(code, "\\t"); break;
            case '\\': anvil_strbuf_append(code, "\\\\"); break;
            case '"':  anvil_strbuf_append(code, "\\\""); break;
            default:   anvil_strbuf_append_char(code, *p); break;
        }
    }
    anvil_strbuf_append(code, "\"\n");
}

static void x64_emit_rodata(x64_mir_emit_t *emit)
{
    size_t count = anvil_mir_num_string_literals(emit->mir);
    if (count == 0 && !emit->emitted_fneg_mask && !emit->emitted_fabs_mask) {
        return;
    }

    if (emit->desc->is_darwin) {
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
        x64_emit_escaped_string(&emit->code, info.value);
    }

    if (emit->emitted_fneg_mask) {
        anvil_strbuf_append(&emit->code, "\t.p2align 4\n");
        anvil_strbuf_append(&emit->code, ".Lx64_fneg_mask32:\n");
        anvil_strbuf_append(&emit->code, "\t.long 0x80000000\n");
        anvil_strbuf_append(&emit->code, "\t.long 0\n\t.long 0\n\t.long 0\n");
        anvil_strbuf_append(&emit->code, ".Lx64_fneg_mask64:\n");
        anvil_strbuf_append(&emit->code, "\t.quad 0x8000000000000000\n");
        anvil_strbuf_append(&emit->code, "\t.quad 0\n");
    }
    if (emit->emitted_fabs_mask) {
        anvil_strbuf_append(&emit->code, "\t.p2align 4\n");
        anvil_strbuf_append(&emit->code, ".Lx64_fabs_mask32:\n");
        anvil_strbuf_append(&emit->code, "\t.long 0x7fffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, ".Lx64_fabs_mask64:\n");
        anvil_strbuf_append(&emit->code, "\t.quad 0x7fffffffffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.quad 0xffffffffffffffff\n");
    }
}

bool anvil_x86_64_emit_mir_abi(const anvil_mir_func_t *mir,
                               anvil_abi_t abi,
                               anvil_syntax_t syntax,
                               char **output,
                               size_t *len)
{
    if (!mir || !output) return false;
    if (!anvil_x86_64_verify_mir_legal(mir, NULL, 0)) return false;

    const anvil_x64_abi_desc_t *desc = anvil_x64_get_abi_desc(abi);
    if (!desc) return false;

    x64_mir_emit_t emit;
    memset(&emit, 0, sizeof(emit));
    emit.mir = mir;
    emit.desc = desc;
    emit.syntax = syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS : syntax;
    anvil_strbuf_init(&emit.code);
    if (!emit.code.data) return false;

    if (!x64_prepare_frame(&emit)) {
        anvil_strbuf_destroy(&emit.code);
        free(emit.spill_offsets);
        free(emit.frame_slot_offsets);
        return false;
    }

    x64_emit_prologue(&emit);

    size_t num_blocks = anvil_mir_num_blocks(mir);
    size_t num_instrs = anvil_mir_num_instrs(mir);
    for (size_t b = 0; b < num_blocks && !emit.failed; b++) {
        if (!x64_emit_label(&emit, (anvil_mir_block_t)b)) {
            emit.failed = true;
            break;
        }

        for (size_t i = 0; i < num_instrs && !emit.failed; i++) {
            anvil_mir_instr_info_t info;
            if (!anvil_mir_get_instr_info(mir, i, &info)) {
                emit.failed = true;
                break;
            }
            if (info.block != (anvil_mir_block_t)b) continue;
            x64_emit_instr(&emit, i, &info);
        }
    }

    if (!emit.failed && !emit.desc->is_darwin) {
        const char *prefix = x64_symbol_prefix(&emit);
        const char *name = anvil_mir_func_name(mir);
        anvil_strbuf_appendf(&emit.code, "\t.size %s%s, .-%s%s\n",
                             prefix, name, prefix, name);
    }
    if (!emit.failed) {
        x64_emit_rodata(&emit);
    }

    free(emit.spill_offsets);
    free(emit.frame_slot_offsets);
    if (emit.failed) {
        anvil_strbuf_destroy(&emit.code);
        return false;
    }

    *output = anvil_strbuf_detach(&emit.code, len);
    return *output != NULL;
}

bool anvil_x86_64_emit_mir(const anvil_mir_func_t *mir,
                           char **output,
                           size_t *len)
{
    return anvil_x86_64_emit_mir_abi(mir, ANVIL_ABI_SYSV, ANVIL_SYNTAX_GAS,
                                     output, len);
}
