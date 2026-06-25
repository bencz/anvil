/*
 * ANVIL - x86 (32-bit) lowering to target-independent MachineIR.
 *
 * x86 follows the shared MachineIR/regalloc path used by the ARM64 and x86-64
 * reference backends. This file lowers source IR into MachineIR, models the
 * calling convention (cdecl, stdcall, fastcall) and stack constraints through a
 * target descriptor, runs allocation/spill materialization, and emits 32-bit
 * x86 assembly (GAS / AT&T) from the allocated machine instructions. 64-bit
 * integers are legalized into lo/hi register pairs (little-endian).
 */

#include "anvil/anvil_x86_mir.h"
#include "anvil/anvil_internal.h"
#include "x86_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int x86_fastcall_int_args[] = { X86_ECX, X86_EDX };
static const int x86_alloc_gpr[] = { X86_EBX, X86_ESI, X86_EDI };
static const int x86_scratch_gpr[] = { X86_EAX, X86_ECX, X86_EDX };
static const int x86_scratch_fpr[] = { 5, 6, 7 };

static const anvil_x86_cc_desc_t x86_cc_descs[] = {
    {
        .cc = ANVIL_CC_CDECL,
        .name = "cdecl",
        .callee_cleans_stack = false,
        .num_reg_int_args = 0,
        .reg_int_args = NULL,
        .decor = X86_DECOR_NONE,
        .int_ret_reg = X86_EAX,
        .int_ret_hi_reg = X86_EDX,
        .alloc_gpr_regs = x86_alloc_gpr,
        .num_alloc_gpr_regs = (int)(sizeof(x86_alloc_gpr) / sizeof(int)),
        .alloc_fpr_regs = NULL,
        .num_alloc_fpr_regs = 0,
        .scratch_gpr_regs = x86_scratch_gpr,
        .num_scratch_gpr_regs = (int)(sizeof(x86_scratch_gpr) / sizeof(int)),
        .scratch_fpr_regs = x86_scratch_fpr,
        .num_scratch_fpr_regs = (int)(sizeof(x86_scratch_fpr) / sizeof(int)),
    },
    {
        .cc = ANVIL_CC_STDCALL,
        .name = "stdcall",
        .callee_cleans_stack = true,
        .num_reg_int_args = 0,
        .reg_int_args = NULL,
        .decor = X86_DECOR_STDCALL,
        .int_ret_reg = X86_EAX,
        .int_ret_hi_reg = X86_EDX,
        .alloc_gpr_regs = x86_alloc_gpr,
        .num_alloc_gpr_regs = (int)(sizeof(x86_alloc_gpr) / sizeof(int)),
        .alloc_fpr_regs = NULL,
        .num_alloc_fpr_regs = 0,
        .scratch_gpr_regs = x86_scratch_gpr,
        .num_scratch_gpr_regs = (int)(sizeof(x86_scratch_gpr) / sizeof(int)),
        .scratch_fpr_regs = x86_scratch_fpr,
        .num_scratch_fpr_regs = (int)(sizeof(x86_scratch_fpr) / sizeof(int)),
    },
    {
        .cc = ANVIL_CC_FASTCALL,
        .name = "fastcall",
        .callee_cleans_stack = true,
        .num_reg_int_args = 2,
        .reg_int_args = x86_fastcall_int_args,
        .decor = X86_DECOR_FASTCALL,
        .int_ret_reg = X86_EAX,
        .int_ret_hi_reg = X86_EDX,
        .alloc_gpr_regs = x86_alloc_gpr,
        .num_alloc_gpr_regs = (int)(sizeof(x86_alloc_gpr) / sizeof(int)),
        .alloc_fpr_regs = NULL,
        .num_alloc_fpr_regs = 0,
        .scratch_gpr_regs = x86_scratch_gpr,
        .num_scratch_gpr_regs = (int)(sizeof(x86_scratch_gpr) / sizeof(int)),
        .scratch_fpr_regs = x86_scratch_fpr,
        .num_scratch_fpr_regs = (int)(sizeof(x86_scratch_fpr) / sizeof(int)),
    },
};

static const anvil_x86_plat_desc_t x86_plat_descs[] = {
    { .platform = X86_PLAT_ELF,   .sym_prefix = "",  .is_macho = false, .is_coff = false },
    { .platform = X86_PLAT_MACHO, .sym_prefix = "_", .is_macho = true,  .is_coff = false },
    { .platform = X86_PLAT_COFF,  .sym_prefix = "_", .is_macho = false, .is_coff = true  },
};

const anvil_x86_cc_desc_t *anvil_x86_get_cc_desc(anvil_cc_t cc)
{
    if (cc == ANVIL_CC_DEFAULT) cc = ANVIL_CC_CDECL;
    if (cc == ANVIL_CC_SYSV || cc == ANVIL_CC_WIN64 ||
        cc == ANVIL_CC_MVS || cc == ANVIL_CC_XPLINK) {
        cc = ANVIL_CC_CDECL;
    }
    for (size_t i = 0; i < sizeof(x86_cc_descs) / sizeof(x86_cc_descs[0]); i++) {
        if (x86_cc_descs[i].cc == cc) return &x86_cc_descs[i];
    }
    return &x86_cc_descs[0];
}

const anvil_x86_plat_desc_t *anvil_x86_get_plat_desc(anvil_abi_t abi)
{
    if (abi == ANVIL_ABI_DARWIN) return &x86_plat_descs[1];
    if (abi == ANVIL_ABI_WIN64) return &x86_plat_descs[2];
    return &x86_plat_descs[0];
}

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

static bool x86_type_is_fp(anvil_type_t *type)
{
    return type && (type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64);
}

static bool x86_type_is_void(anvil_type_t *type)
{
    return !type || type->kind == ANVIL_TYPE_VOID;
}

static bool x86_type_is_signed(anvil_type_t *type)
{
    return type && type->is_signed && !x86_type_is_fp(type);
}

static bool x86_type_is_i64(anvil_type_t *type)
{
    return type && (type->kind == ANVIL_TYPE_I64 || type->kind == ANVIL_TYPE_U64);
}

static bool x86_type_is_small_aggregate_pair(anvil_type_t *type)
{
    if (!type) return false;
    if (type->kind != ANVIL_TYPE_STRUCT && type->kind != ANVIL_TYPE_ARRAY) {
        return false;
    }
    size_t size = anvil_type_size(type);
    return size > 4 && size <= 8;
}

static bool x86_needs_pair(anvil_type_t *type)
{
    return x86_type_is_i64(type) || x86_type_is_small_aggregate_pair(type);
}

static anvil_mir_reg_class_t x86_reg_class_for_type(anvil_type_t *type)
{
    return x86_type_is_fp(type) ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR;
}

static uint16_t x86_bits_for_type(anvil_type_t *type)
{
    if (!type) return 32;
    if (type->kind == ANVIL_TYPE_PTR) return 32;
    if (x86_type_is_fp(type)) {
        return type->kind == ANVIL_TYPE_F64 ? 64 : 32;
    }

    size_t size = anvil_type_size(type);
    if (size == 0) return 32;
    if (size >= 4) return 32;
    return (uint16_t)(size * 8);
}

static uint16_t x86_slot_bits_for_type(anvil_type_t *type)
{
    size_t size = type ? x86_type_size(type) : 4;
    if (size == 0) size = 4;
    if (size > UINT16_MAX / 8) size = UINT16_MAX / 8;
    return (uint16_t)(size * 8);
}

static uint16_t x86_align_for_type(anvil_type_t *type)
{
    int align = type ? x86_type_align(type) : 4;
    if (align <= 0) align = 4;
    if (align > UINT16_MAX) align = UINT16_MAX;
    return (uint16_t)align;
}

static anvil_mir_vreg_t x86_add_vreg_for_type(x86_mir_lower_t *lower,
                                              anvil_type_t *type)
{
    return anvil_mir_add_vreg_typed(lower->mir,
                                    x86_reg_class_for_type(type),
                                    x86_bits_for_type(type),
                                    x86_type_is_signed(type));
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

static bool map_put(x86_mir_lower_t *lower, anvil_value_t *value,
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

static anvil_mir_vreg_t map_get(x86_mir_lower_t *lower, anvil_value_t *value)
{
    for (size_t i = 0; i < lower->num_values; i++) {
        if (lower->values[i].value == value) return lower->values[i].vreg;
    }
    return ANVIL_MIR_NO_VREG;
}

static bool pair_reserve(x86_mir_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_pairs) return true;
    size_t new_cap = lower->cap_pairs ? lower->cap_pairs * 2 : 16;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) return false;
        new_cap *= 2;
    }
    value_pair_t *grown = realloc(lower->pairs, new_cap * sizeof(*grown));
    if (!grown) return false;
    lower->pairs = grown;
    lower->cap_pairs = new_cap;
    return true;
}

static bool pair_put(x86_mir_lower_t *lower, anvil_value_t *value,
                     anvil_mir_vreg_t hi, anvil_mir_vreg_t lo, bool is_unsigned)
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
    if (!pair_reserve(lower, lower->num_pairs + 1)) return false;
    lower->pairs[lower->num_pairs].value = value;
    lower->pairs[lower->num_pairs].hi = hi;
    lower->pairs[lower->num_pairs].lo = lo;
    lower->pairs[lower->num_pairs].is_unsigned = is_unsigned;
    lower->num_pairs++;
    return true;
}

static bool pair_get(x86_mir_lower_t *lower, anvil_value_t *value,
                     anvil_mir_vreg_t *hi, anvil_mir_vreg_t *lo,
                     bool *is_unsigned)
{
    if (!value) return false;
    for (size_t i = 0; i < lower->num_pairs; i++) {
        if (lower->pairs[i].value == value) {
            if (hi) *hi = lower->pairs[i].hi;
            if (lo) *lo = lower->pairs[i].lo;
            if (is_unsigned) *is_unsigned = lower->pairs[i].is_unsigned;
            return true;
        }
    }
    return false;
}

static bool wide_const_reserve(x86_mir_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_wide_consts) return true;
    size_t new_cap = lower->cap_wide_consts ? lower->cap_wide_consts * 2 : 16;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) return false;
        new_cap *= 2;
    }
    value_wide_const_t *grown =
        realloc(lower->wide_consts, new_cap * sizeof(*grown));
    if (!grown) return false;
    lower->wide_consts = grown;
    lower->cap_wide_consts = new_cap;
    return true;
}

static bool wide_const_put(x86_mir_lower_t *lower, anvil_value_t *value,
                           int64_t imm)
{
    if (!value) return false;
    for (size_t i = 0; i < lower->num_wide_consts; i++) {
        if (lower->wide_consts[i].value == value) {
            lower->wide_consts[i].imm = imm;
            lower->wide_consts[i].valid = true;
            return true;
        }
    }
    if (!wide_const_reserve(lower, lower->num_wide_consts + 1)) return false;
    lower->wide_consts[lower->num_wide_consts].value = value;
    lower->wide_consts[lower->num_wide_consts].imm = imm;
    lower->wide_consts[lower->num_wide_consts].valid = true;
    lower->num_wide_consts++;
    return true;
}

static bool wide_const_get(x86_mir_lower_t *lower, anvil_value_t *value,
                           int64_t *out_imm)
{
    if (value && value->kind == ANVIL_VAL_CONST_INT) {
        if (out_imm) *out_imm = value->data.i;
        return true;
    }
    if (!value) return false;
    for (size_t i = 0; i < lower->num_wide_consts; i++) {
        if (lower->wide_consts[i].value == value &&
            lower->wide_consts[i].valid) {
            if (out_imm) *out_imm = lower->wide_consts[i].imm;
            return true;
        }
    }
    return false;
}

static bool addr_map_reserve(x86_mir_lower_t *lower, size_t needed)
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

static bool addr_map_put(x86_mir_lower_t *lower,
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

static bool addr_map_get(x86_mir_lower_t *lower,
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

static anvil_mir_block_t block_get(x86_mir_lower_t *lower,
                                   anvil_block_t *block)
{
    if (!block) return ANVIL_MIR_NO_BLOCK;
    for (size_t i = 0; i < lower->num_blocks; i++) {
        if (lower->blocks[i].block == block) return lower->blocks[i].mir_block;
    }
    return ANVIL_MIR_NO_BLOCK;
}

static bool create_mir_blocks(x86_mir_lower_t *lower)
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

static int64_t x86_stack_arg_slot_size(anvil_type_t *type)
{
    int64_t size = type ? x86_type_size(type) : 4;
    if (size <= 0) size = 4;
    return (size + 3) & ~INT64_C(3);
}

static bool x86_arg_uses_int_reg(const anvil_x86_cc_desc_t *desc,
                                 anvil_type_t *type,
                                 size_t int_reg_count)
{
    if (x86_type_is_fp(type) || x86_needs_pair(type)) return false;
    return (int)int_reg_count < desc->num_reg_int_args;
}

static bool lower_params(x86_mir_lower_t *lower)
{
    const anvil_x86_cc_desc_t *desc = lower->desc;
    int64_t stack_offset = 0;
    size_t int_reg_count = 0;

    anvil_mir_block_t entry = block_get(lower, lower->func->blocks);
    if (entry == ANVIL_MIR_NO_BLOCK ||
        !anvil_mir_set_current_block(lower->mir, entry)) {
        return false;
    }

    for (size_t i = 0; i < lower->func->num_params; i++) {
        anvil_value_t *param = lower->func->params[i];
        if (!param) return false;

        if (x86_needs_pair(param->type)) {
            bool is_unsigned = param->type->kind == ANVIL_TYPE_U64;
            anvil_mir_vreg_t lo = x86_add_i32_vreg(lower, false);
            anvil_mir_vreg_t hi = x86_add_i32_vreg(lower, !is_unsigned);
            if (lo == ANVIL_MIR_NO_VREG || hi == ANVIL_MIR_NO_VREG) return false;
            if (!anvil_mir_add_instr_imm(lower->mir,
                                         ANVIL_MIR_OP_INCOMING_STACK_ARG,
                                         lo, stack_offset)) {
                return false;
            }
            if (!anvil_mir_add_instr_imm(lower->mir,
                                         ANVIL_MIR_OP_INCOMING_STACK_ARG,
                                         hi, stack_offset + 4)) {
                return false;
            }
            stack_offset += 8;
            if (!pair_put(lower, param, hi, lo, is_unsigned)) return false;
            continue;
        }

        anvil_mir_vreg_t local = x86_add_vreg_for_type(lower, param->type);
        if (local == ANVIL_MIR_NO_VREG) return false;

        if (x86_arg_uses_int_reg(desc, param->type, int_reg_count)) {
            int reg = desc->reg_int_args[int_reg_count];
            anvil_mir_vreg_t incoming = x86_add_vreg_for_type(lower, param->type);
            if (incoming == ANVIL_MIR_NO_VREG ||
                !anvil_mir_set_fixed_reg(lower->mir, incoming, reg)) {
                return false;
            }
            anvil_mir_vreg_t uses[] = { incoming };
            if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                                     local, uses, 1)) {
                return false;
            }
            int_reg_count++;
        } else {
            if (!anvil_mir_add_instr_imm(lower->mir,
                                         ANVIL_MIR_OP_INCOMING_STACK_ARG,
                                         local, stack_offset)) {
                return false;
            }
            stack_offset += x86_stack_arg_slot_size(param->type);
        }

        if (!map_put(lower, param, local)) return false;
    }

    return true;
}

static bool prepare_phi_results(x86_mir_lower_t *lower)
{
    for (anvil_block_t *block = lower->func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_PHI) break;
            if (!instr->result) return false;
            if (x86_needs_pair(instr->result->type)) return false;

            anvil_mir_vreg_t vreg = x86_add_vreg_for_type(lower,
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

static anvil_mir_vreg_t lower_value(x86_mir_lower_t *lower,
                                    anvil_value_t *value);
static bool lower_add_const_offset(x86_mir_lower_t *lower,
                                   anvil_mir_vreg_t base,
                                   int64_t offset,
                                   anvil_mir_vreg_t *out_ptr);
static bool emit_vreg_copy(anvil_mir_func_t *mir,
                           anvil_mir_vreg_t dst,
                           anvil_mir_vreg_t src);

static anvil_mir_vreg_t lower_const_value(x86_mir_lower_t *lower,
                                          anvil_value_t *value)
{
    anvil_mir_vreg_t vreg = x86_add_vreg_for_type(lower, value->type);
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
    if (pair_get(lower, value, &exist_hi, &exist_lo, NULL)) return true;
    if (value->kind != ANVIL_VAL_CONST_INT) return false;

    int64_t imm = value->data.i;
    uint64_t bits = (uint64_t)imm;
    int32_t hi_imm = (int32_t)(bits >> 32);
    int32_t lo_imm = (int32_t)(bits & 0xffffffffu);
    bool is_unsigned = value->type && value->type->kind == ANVIL_TYPE_U64;

    anvil_mir_vreg_t lo = x86_add_i32_vreg(lower, false);
    anvil_mir_vreg_t hi = x86_add_i32_vreg(lower, !is_unsigned);
    if (lo == ANVIL_MIR_NO_VREG || hi == ANVIL_MIR_NO_VREG) return false;

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, lo,
                                 (int64_t)lo_imm) ||
        !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, hi,
                                 (int64_t)hi_imm)) {
        return false;
    }

    return wide_const_put(lower, value, imm) &&
           pair_put(lower, value, hi, lo, is_unsigned);
}

static bool ensure_i64_pair(x86_mir_lower_t *lower, anvil_value_t *value,
                            anvil_mir_vreg_t *hi, anvil_mir_vreg_t *lo,
                            bool *is_unsigned)
{
    if (pair_get(lower, value, hi, lo, is_unsigned)) return true;
    if (value && value->kind == ANVIL_VAL_CONST_INT &&
        lower_i64_const_pair(lower, value)) {
        return pair_get(lower, value, hi, lo, is_unsigned);
    }
    return false;
}

static anvil_mir_vreg_t lower_symbol_address(x86_mir_lower_t *lower,
                                             const char *symbol)
{
    if (!symbol || !symbol[0]) return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t vreg =
        anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, false);
    if (vreg == ANVIL_MIR_NO_VREG) return ANVIL_MIR_NO_VREG;

    if (!anvil_mir_add_instr_symbol(lower->mir, ANVIL_MIR_OP_SYMBOL_ADDR,
                                    vreg, NULL, 0, symbol)) {
        return ANVIL_MIR_NO_VREG;
    }
    return vreg;
}

static anvil_mir_vreg_t lower_string_address(x86_mir_lower_t *lower,
                                             anvil_value_t *value)
{
    const char *label = NULL;
    if (anvil_mir_add_string_literal(lower->mir, value->data.str,
                                     &label) < 0 || !label) {
        return ANVIL_MIR_NO_VREG;
    }
    return lower_symbol_address(lower, label);
}

static anvil_mir_vreg_t lower_value(x86_mir_lower_t *lower,
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

static bool add_return(x86_mir_lower_t *lower, anvil_value_t *value)
{
    if (!value) {
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET,
                                   ANVIL_MIR_NO_VREG, NULL, 0);
    }

    if (x86_needs_pair(value->type)) {
        anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
        anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
        if (!ensure_i64_pair(lower, value, &hi, &lo, NULL)) return false;

        anvil_mir_vreg_t hi_use[] = { hi };
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_OTHER,
                                 ANVIL_MIR_NO_VREG, hi_use, 1)) {
            return false;
        }
        anvil_mir_vreg_t uses[] = { lo };
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET,
                                   ANVIL_MIR_NO_VREG, uses, 1);
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

static bool break_parallel_copy_cycle(x86_mir_lower_t *lower,
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

static bool emit_parallel_phi_edge_copies(x86_mir_lower_t *lower,
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

static bool lower_phi_copies_for_edge(x86_mir_lower_t *lower,
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

static anvil_mir_block_t create_phi_edge_block(x86_mir_lower_t *lower,
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

static bool emit_phi_edge_block(x86_mir_lower_t *lower,
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

static bool prepare_phi_aware_target(x86_mir_lower_t *lower,
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

static bool emit_pending_phi_edges(x86_mir_lower_t *lower,
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

static anvil_mir_block_t create_switch_chain_block(x86_mir_lower_t *lower,
                                                   anvil_block_t *src_block,
                                                   size_t case_index)
{
    const char *src_name = (src_block && src_block->name) ? src_block->name : "anon";

    char name[256];
    snprintf(name, sizeof(name), "%s_switch_case_%zu_%zu",
             src_name, case_index, lower->num_edge_blocks++);
    return anvil_mir_add_block(lower->mir, name);
}

static bool lower_call(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    const anvil_x86_cc_desc_t *desc = lower->desc;
    if (instr->num_operands == 0) return false;

    anvil_value_t *callee = instr->operands[0];
    anvil_type_t *fn_type = call_func_type(callee);
    if (!fn_type) return false;

    bool direct_call = call_is_direct_symbol(callee);
    const char *symbol = direct_call ? call_symbol(callee) : NULL;
    if (direct_call && !symbol) return false;

    size_t num_args = instr->num_operands - 1;
    size_t max_call_uses = num_args + (direct_call ? 0 : 1) + 1;
    anvil_mir_vreg_t *call_uses = NULL;
    if (max_call_uses > 0) {
        call_uses = calloc(max_call_uses, sizeof(*call_uses));
        if (!call_uses) return false;
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
            anvil_mir_vreg_t lo_use[] = { lo };
            anvil_mir_vreg_t hi_use[] = { hi };
            if (!anvil_mir_add_instr_imm_uses(lower->mir,
                                              ANVIL_MIR_OP_CALL_STACK_ARG,
                                              ANVIL_MIR_NO_VREG,
                                              lo_use, 1, stack_offset) ||
                !anvil_mir_add_instr_imm_uses(lower->mir,
                                              ANVIL_MIR_OP_CALL_STACK_ARG,
                                              ANVIL_MIR_NO_VREG,
                                              hi_use, 1, stack_offset + 4)) {
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

        anvil_mir_vreg_t stack_use[] = { src };
        if (!anvil_mir_add_instr_imm_uses(lower->mir,
                                          ANVIL_MIR_OP_CALL_STACK_ARG,
                                          ANVIL_MIR_NO_VREG,
                                          stack_use, 1, stack_offset)) {
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
    bool result_is_fp = instr->result && x86_type_is_fp(instr->result->type);
    bool result_is_int = instr->result &&
                         !x86_type_is_void(instr->result->type) &&
                         !result_is_pair && !result_is_fp;

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

    bool emit_ok = anvil_mir_add_instr_symbol(lower->mir, ANVIL_MIR_OP_CALL,
                                              call_def, call_uses,
                                              num_call_uses, symbol);
    free(call_uses);
    if (!emit_ok) return false;

    if (!instr->result || x86_type_is_void(instr->result->type)) {
        return true;
    }

    if (result_is_pair) {
        bool is_unsigned = instr->result->type->kind == ANVIL_TYPE_U64;
        anvil_mir_vreg_t hi = x86_add_i32_vreg(lower, !is_unsigned);
        if (hi == ANVIL_MIR_NO_VREG) return false;
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_OTHER, hi, NULL, 0)) {
            return false;
        }
        return pair_put(lower, instr->result, hi, call_def, is_unsigned);
    }

    if (result_is_fp) {
        anvil_mir_vreg_t local_result =
            x86_add_vreg_for_type(lower, instr->result->type);
        if (local_result == ANVIL_MIR_NO_VREG) return false;
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_OTHER,
                                 local_result, NULL, 0)) {
            return false;
        }
        return map_put(lower, instr->result, local_result);
    }

    return map_put(lower, instr->result, call_def);
}

static anvil_mir_vreg_t lower_widen_gpr_to_32(x86_mir_lower_t *lower,
                                              anvil_mir_vreg_t src,
                                              bool sign_extend)
{
    const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->mir, src);
    if (!src_info || src_info->reg_class != ANVIL_MIR_REG_GPR) {
        return ANVIL_MIR_NO_VREG;
    }
    if (src_info->size_bits >= 32) return src;

    anvil_mir_vreg_t wide =
        anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, sign_extend);
    if (wide == ANVIL_MIR_NO_VREG) return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t uses[] = { src };
    anvil_mir_opcode_t op = sign_extend ? ANVIL_MIR_OP_SEXT : ANVIL_MIR_OP_ZEXT;
    if (!anvil_mir_add_instr(lower->mir, op, wide, uses, 1)) {
        return ANVIL_MIR_NO_VREG;
    }
    return wide;
}

static anvil_mir_vreg_t lower_resize_gpr(x86_mir_lower_t *lower,
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

static bool lower_match_binary_operand_sizes(x86_mir_lower_t *lower,
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

static bool lower_alloca(x86_mir_lower_t *lower, anvil_instr_t *instr)
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

    anvil_mir_vreg_t ptr = x86_add_vreg_for_type(lower, instr->result->type);
    if (ptr == ANVIL_MIR_NO_VREG) return false;

    if (instr->num_operands == 0) {
        int slot = anvil_mir_add_frame_slot(lower->mir,
                                            x86_slot_bits_for_type(element_type),
                                            x86_align_for_type(element_type));
        if (slot < 0) return false;
        if (!anvil_mir_add_frame_addr(lower->mir, ptr, slot)) return false;
        return map_put(lower, instr->result, ptr);
    }

    if (instr->num_operands != 1) return false;
    anvil_mir_vreg_t count = lower_value(lower, instr->operands[0]);
    if (count == ANVIL_MIR_NO_VREG) return false;
    count = lower_widen_gpr_to_32(lower, count, false);
    if (count == ANVIL_MIR_NO_VREG) return false;

    int64_t elem_size = x86_type_size(element_type);
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

    int64_t elem_size = element_type ? x86_type_size(element_type) : 1;
    return elem_size > 0 ? elem_size : 1;
}

static bool lower_add_const_offset(x86_mir_lower_t *lower,
                                   anvil_mir_vreg_t base,
                                   int64_t offset,
                                   anvil_mir_vreg_t *out_ptr)
{
    if (offset == 0) {
        *out_ptr = base;
        return true;
    }

    anvil_mir_vreg_t off =
        anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t dst =
        anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 32);
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

static bool lower_gep(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands < 1 || !instr->result) return false;

    anvil_mir_vreg_t current = lower_value(lower, instr->operands[0]);
    if (current == ANVIL_MIR_NO_VREG) return false;
    current = lower_widen_gpr_to_32(lower, current, false);
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
        index = lower_widen_gpr_to_32(lower, index, true);
        if (index == ANVIL_MIR_NO_VREG) return false;

        anvil_mir_vreg_t scaled = index;
        if (elem_size != 1) {
            anvil_mir_vreg_t scale =
                anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 32);
            scaled = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 32);
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
            anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, 32);
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

static bool lower_struct_gep(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands < 1 || !instr->result) return false;

    anvil_mir_vreg_t base = lower_value(lower, instr->operands[0]);
    if (base == ANVIL_MIR_NO_VREG) return false;
    base = lower_widen_gpr_to_32(lower, base, false);
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

static bool lower_cast(x86_mir_lower_t *lower,
                       anvil_instr_t *instr,
                       anvil_mir_opcode_t mir_op)
{
    if (instr->num_operands != 1 || !instr->result) return false;

    bool dst_pair = x86_needs_pair(instr->result->type);
    bool src_pair = x86_needs_pair(instr->operands[0]->type);

    if (dst_pair) {
        if (mir_op != ANVIL_MIR_OP_BITCAST) return false;
        anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
        anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
        bool is_unsigned = instr->result->type->kind == ANVIL_TYPE_U64;
        if (!ensure_i64_pair(lower, instr->operands[0], &hi, &lo, NULL)) {
            return false;
        }
        int64_t imm = 0;
        if (wide_const_get(lower, instr->operands[0], &imm) &&
            !wide_const_put(lower, instr->result, imm)) {
            return false;
        }
        return pair_put(lower, instr->result, hi, lo, is_unsigned);
    }

    if (src_pair) return false;

    anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t def = x86_add_vreg_for_type(lower, instr->result->type);
    if (src == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) return false;

    anvil_mir_vreg_t uses[] = { src };
    if (!anvil_mir_add_instr(lower->mir, mir_op, def, uses, 1)) return false;
    return map_put(lower, instr->result, def);
}

static bool lower_select(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands != 3 || !instr->result) return false;
    if (x86_needs_pair(instr->result->type)) return false;

    anvil_mir_vreg_t cond = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t then_v = lower_value(lower, instr->operands[1]);
    anvil_mir_vreg_t else_v = lower_value(lower, instr->operands[2]);
    anvil_mir_vreg_t def = x86_add_vreg_for_type(lower, instr->result->type);
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

static bool lower_memory_address(x86_mir_lower_t *lower,
                                 anvil_value_t *value,
                                 anvil_mir_vreg_t *out_base,
                                 int64_t *out_offset)
{
    anvil_mir_vreg_t base = ANVIL_MIR_NO_VREG;
    int64_t offset = 0;
    if (addr_map_get(lower, value, &base, &offset)) {
        base = lower_widen_gpr_to_32(lower, base, false);
        if (base == ANVIL_MIR_NO_VREG) return false;
        *out_base = base;
        *out_offset = offset;
        return true;
    }

    base = lower_value(lower, value);
    if (base == ANVIL_MIR_NO_VREG) return false;
    base = lower_widen_gpr_to_32(lower, base, false);
    if (base == ANVIL_MIR_NO_VREG) return false;

    *out_base = base;
    *out_offset = 0;
    return true;
}

static bool lower_load_i64_pair(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || instr->num_operands != 1) return false;

    anvil_mir_vreg_t base = ANVIL_MIR_NO_VREG;
    int64_t offset = 0;
    if (!lower_memory_address(lower, instr->operands[0], &base, &offset)) {
        return false;
    }
    if (offset > INT64_MAX - 4) return false;

    bool is_unsigned = instr->result->type &&
                       instr->result->type->kind == ANVIL_TYPE_U64;
    anvil_mir_vreg_t lo = x86_add_i32_vreg(lower, false);
    anvil_mir_vreg_t hi = x86_add_i32_vreg(lower, !is_unsigned);
    if (lo == ANVIL_MIR_NO_VREG || hi == ANVIL_MIR_NO_VREG) return false;

    anvil_mir_vreg_t lo_uses[] = { base };
    bool ok = offset == 0
        ? anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_LOAD, lo, lo_uses, 1)
        : anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_LOAD,
                                       lo, lo_uses, 1, offset);
    if (!ok) return false;

    anvil_mir_vreg_t hi_uses[] = { base };
    if (!anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_LOAD,
                                      hi, hi_uses, 1, offset + 4)) {
        return false;
    }

    return pair_put(lower, instr->result, hi, lo, is_unsigned);
}

static bool lower_store_i64_pair(x86_mir_lower_t *lower,
                                 anvil_value_t *value,
                                 anvil_mir_vreg_t base,
                                 int64_t offset)
{
    if (offset > INT64_MAX - 4) return false;

    anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
    if (!ensure_i64_pair(lower, value, &hi, &lo, NULL)) return false;

    anvil_mir_vreg_t lo_uses[] = { lo, base };
    bool ok = offset == 0
        ? anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_STORE,
                              ANVIL_MIR_NO_VREG, lo_uses, 2)
        : anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_STORE,
                                       ANVIL_MIR_NO_VREG, lo_uses, 2, offset);
    if (!ok) return false;

    anvil_mir_vreg_t hi_uses[] = { hi, base };
    return anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_STORE,
                                        ANVIL_MIR_NO_VREG, hi_uses, 2,
                                        offset + 4);
}

static bool add_pair_cmp(x86_mir_lower_t *lower, anvil_mir_opcode_t op,
                         anvil_mir_vreg_t def, anvil_mir_vreg_t a,
                         anvil_mir_vreg_t b)
{
    anvil_mir_vreg_t uses[] = { a, b };
    return anvil_mir_add_instr(lower->mir, op, def, uses, 2);
}

static bool lower_i64_cmp_pair(x86_mir_lower_t *lower, anvil_instr_t *instr,
                               anvil_mir_opcode_t op)
{
    if (!instr->result || instr->num_operands != 2) return false;

    anvil_mir_vreg_t lhi = ANVIL_MIR_NO_VREG, llo = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t rhi = ANVIL_MIR_NO_VREG, rlo = ANVIL_MIR_NO_VREG;
    if (!ensure_i64_pair(lower, instr->operands[0], &lhi, &llo, NULL) ||
        !ensure_i64_pair(lower, instr->operands[1], &rhi, &rlo, NULL)) {
        return false;
    }

    anvil_mir_vreg_t result = x86_add_bool_vreg(lower);
    if (result == ANVIL_MIR_NO_VREG) return false;

    if (op == ANVIL_MIR_OP_CMP_EQ || op == ANVIL_MIR_OP_CMP_NE) {
        anvil_mir_vreg_t hi_cmp = x86_add_bool_vreg(lower);
        anvil_mir_vreg_t lo_cmp = x86_add_bool_vreg(lower);
        if (hi_cmp == ANVIL_MIR_NO_VREG || lo_cmp == ANVIL_MIR_NO_VREG) {
            return false;
        }
        if (!add_pair_cmp(lower, op, hi_cmp, lhi, rhi) ||
            !add_pair_cmp(lower, op, lo_cmp, llo, rlo)) {
            return false;
        }
        anvil_mir_vreg_t uses[] = { hi_cmp, lo_cmp };
        anvil_mir_opcode_t join = op == ANVIL_MIR_OP_CMP_EQ
                                      ? ANVIL_MIR_OP_AND : ANVIL_MIR_OP_OR;
        if (!anvil_mir_add_instr(lower->mir, join, result, uses, 2)) {
            return false;
        }
        return map_put(lower, instr->result, result);
    }

    bool unsigned_cmp = op == ANVIL_MIR_OP_CMP_ULT ||
                        op == ANVIL_MIR_OP_CMP_ULE ||
                        op == ANVIL_MIR_OP_CMP_UGT ||
                        op == ANVIL_MIR_OP_CMP_UGE;
    bool less = op == ANVIL_MIR_OP_CMP_LT || op == ANVIL_MIR_OP_CMP_LE ||
                op == ANVIL_MIR_OP_CMP_ULT || op == ANVIL_MIR_OP_CMP_ULE;
    bool equal_ok = op == ANVIL_MIR_OP_CMP_LE || op == ANVIL_MIR_OP_CMP_GE ||
                    op == ANVIL_MIR_OP_CMP_ULE || op == ANVIL_MIR_OP_CMP_UGE;

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
    if (hi_cmp == ANVIL_MIR_NO_VREG || hi_eq == ANVIL_MIR_NO_VREG ||
        lo_cmp == ANVIL_MIR_NO_VREG || eq_and_lo == ANVIL_MIR_NO_VREG) {
        return false;
    }

    if (!add_pair_cmp(lower, hi_rel, hi_cmp, lhi, rhi) ||
        !add_pair_cmp(lower, ANVIL_MIR_OP_CMP_EQ, hi_eq, lhi, rhi) ||
        !add_pair_cmp(lower, lo_rel, lo_cmp, llo, rlo)) {
        return false;
    }

    anvil_mir_vreg_t and_uses[] = { hi_eq, lo_cmp };
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_AND,
                             eq_and_lo, and_uses, 2)) {
        return false;
    }
    anvil_mir_vreg_t or_uses[] = { hi_cmp, eq_and_lo };
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_OR,
                             result, or_uses, 2)) {
        return false;
    }
    return map_put(lower, instr->result, result);
}

static bool lower_i64_bitwise_pair(x86_mir_lower_t *lower, anvil_instr_t *instr,
                                   anvil_mir_opcode_t op)
{
    if (!instr->result || instr->num_operands != 2) return false;

    anvil_mir_vreg_t lhi = ANVIL_MIR_NO_VREG, llo = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t rhi = ANVIL_MIR_NO_VREG, rlo = ANVIL_MIR_NO_VREG;
    bool lu = false;
    if (!ensure_i64_pair(lower, instr->operands[0], &lhi, &llo, &lu) ||
        !ensure_i64_pair(lower, instr->operands[1], &rhi, &rlo, NULL)) {
        return false;
    }

    bool is_unsigned = instr->result->type->kind == ANVIL_TYPE_U64;
    anvil_mir_vreg_t lo = x86_add_i32_vreg(lower, false);
    anvil_mir_vreg_t hi = x86_add_i32_vreg(lower, !is_unsigned);
    if (lo == ANVIL_MIR_NO_VREG || hi == ANVIL_MIR_NO_VREG) return false;

    anvil_mir_vreg_t lo_uses[] = { llo, rlo };
    anvil_mir_vreg_t hi_uses[] = { lhi, rhi };
    if (!anvil_mir_add_instr(lower->mir, op, lo, lo_uses, 2) ||
        !anvil_mir_add_instr(lower->mir, op, hi, hi_uses, 2)) {
        return false;
    }

    return pair_put(lower, instr->result, hi, lo, is_unsigned);
}

static bool lower_i64_unop_const(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    int64_t imm = 0;
    if (!wide_const_get(lower, instr->operands[0], &imm)) return false;

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
    if (lo == ANVIL_MIR_NO_VREG || hi == ANVIL_MIR_NO_VREG) return false;
    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, lo,
                                 (int64_t)lo_imm) ||
        !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, hi,
                                 (int64_t)hi_imm)) {
        return false;
    }
    return wide_const_put(lower, instr->result, result) &&
           pair_put(lower, instr->result, hi, lo, is_unsigned);
}

static bool lower_switch(x86_mir_lower_t *lower, anvil_instr_t *instr)
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

static bool lower_instr(x86_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->op == ANVIL_OP_NOP) return true;
    if (instr->op == ANVIL_OP_PHI) return true;

    anvil_mir_opcode_t mir_op;
    if (instr->num_operands == 2 && map_binop(instr->op, &mir_op)) {
        bool wide = (instr->operands[0] &&
                     x86_needs_pair(instr->operands[0]->type)) ||
                    (instr->operands[1] &&
                     x86_needs_pair(instr->operands[1]->type)) ||
                    (instr->result && x86_needs_pair(instr->result->type));
        if (wide) {
            if (mir_op_is_compare(mir_op)) {
                return lower_i64_cmp_pair(lower, instr, mir_op);
            }
            if (mir_op == ANVIL_MIR_OP_AND || mir_op == ANVIL_MIR_OP_OR ||
                mir_op == ANVIL_MIR_OP_XOR) {
                return lower_i64_bitwise_pair(lower, instr, mir_op);
            }
            return false;
        }

        anvil_mir_vreg_t lhs = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t rhs = lower_value(lower, instr->operands[1]);
        anvil_mir_vreg_t def = instr->result
            ? x86_add_vreg_for_type(lower, instr->result->type)
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
        if (instr->result && x86_needs_pair(instr->result->type)) {
            return lower_i64_unop_const(lower, instr);
        }
        anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t def = instr->result
            ? x86_add_vreg_for_type(lower, instr->result->type)
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
            if (x86_needs_pair(instr->result->type)) {
                return lower_load_i64_pair(lower, instr);
            }
            anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
            int64_t offset = 0;
            if (!lower_memory_address(lower, instr->operands[0],
                                      &ptr, &offset)) {
                return false;
            }
            anvil_mir_vreg_t def = x86_add_vreg_for_type(lower,
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
            anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
            int64_t offset = 0;
            if (!lower_memory_address(lower, instr->operands[1],
                                      &ptr, &offset)) {
                return false;
            }
            if (ptr == ANVIL_MIR_NO_VREG) return false;
            if (instr->operands[0] && x86_needs_pair(instr->operands[0]->type)) {
                return lower_store_i64_pair(lower, instr->operands[0],
                                            ptr, offset);
            }
            anvil_mir_vreg_t val = lower_value(lower, instr->operands[0]);
            if (val == ANVIL_MIR_NO_VREG) return false;
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

static anvil_cc_t x86_lower_cc(anvil_func_t *func)
{
    return func ? func->cc : ANVIL_CC_DEFAULT;
}

anvil_mir_func_t *anvil_x86_lower_func_to_mir(anvil_func_t *func)
{
    if (!func || func->is_declaration) return NULL;

    const anvil_x86_cc_desc_t *desc = anvil_x86_get_cc_desc(x86_lower_cc(func));
    if (!desc) return NULL;

    x86_mir_lower_t lower;
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
        free(lower.pairs);
        free(lower.wide_consts);
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
            free(lower.pairs);
            free(lower.wide_consts);
            free(lower.addr_offsets);
            return NULL;
        }

        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (!lower_instr(&lower, instr)) {
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
    return lower.mir;
}

static bool x86_legal_fail(char *error, size_t error_len,
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

static bool x86_legal_size_for_class(const anvil_mir_vreg_info_t *info)
{
    if (!info) return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR) {
        return info->size_bits == 8 ||
               info->size_bits == 16 ||
               info->size_bits == 32;
    }
    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        return info->size_bits == 32 || info->size_bits == 64;
    }
    return false;
}

static bool x86_legal_fixed_reg(const anvil_mir_vreg_info_t *info)
{
    if (!info || !info->has_fixed_reg) return true;
    if (info->fixed_phys_reg < 0) return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR) return info->fixed_phys_reg <= 7;
    if (info->reg_class == ANVIL_MIR_REG_FPR) return info->fixed_phys_reg <= 7;
    return false;
}

static const anvil_mir_vreg_info_t *x86_legal_vreg_info(
    const anvil_mir_func_t *mir,
    anvil_mir_vreg_t vreg,
    size_t instr_index,
    char *error,
    size_t error_len)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, vreg);
    if (!info) {
        x86_legal_fail(error, error_len,
                       "x86 MIR instruction %zu uses invalid vreg",
                       instr_index);
        return NULL;
    }
    if (!x86_legal_size_for_class(info)) {
        x86_legal_fail(error, error_len,
                       "x86 MIR instruction %zu uses unsupported vreg class/size",
                       instr_index);
        return NULL;
    }
    if (!x86_legal_fixed_reg(info)) {
        x86_legal_fail(error, error_len,
                       "x86 MIR instruction %zu uses invalid fixed register",
                       instr_index);
        return NULL;
    }
    return info;
}

static bool x86_legal_pointer_operand(const anvil_mir_func_t *mir,
                                      anvil_mir_vreg_t vreg,
                                      size_t instr_index,
                                      char *error,
                                      size_t error_len)
{
    const anvil_mir_vreg_info_t *info =
        x86_legal_vreg_info(mir, vreg, instr_index, error, error_len);
    if (!info) return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR && info->size_bits == 32) {
        return true;
    }
    return x86_legal_fail(error, error_len,
                          "x86 MIR instruction %zu requires a 32-bit pointer operand",
                          instr_index);
}

static bool x86_legal_same_class_and_size(const anvil_mir_vreg_info_t *a,
                                          const anvil_mir_vreg_info_t *b)
{
    return a && b &&
           a->reg_class == b->reg_class &&
           a->size_bits == b->size_bits;
}

static bool x86_legal_binary(const anvil_mir_func_t *mir,
                             size_t instr_index,
                             const anvil_mir_instr_info_t *instr,
                             char *error,
                             size_t error_len)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(mir, instr_index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(mir, instr_index, 1);
    const anvil_mir_vreg_info_t *def =
        x86_legal_vreg_info(mir, instr->def, instr_index, error, error_len);
    const anvil_mir_vreg_info_t *lhs_info =
        x86_legal_vreg_info(mir, lhs, instr_index, error, error_len);
    const anvil_mir_vreg_info_t *rhs_info =
        x86_legal_vreg_info(mir, rhs, instr_index, error, error_len);
    if (!def || !lhs_info || !rhs_info) return false;

    bool is_compare = false;
    switch (instr->op) {
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
            is_compare = true;
            break;
        default:
            break;
    }

    if (is_compare) {
        if (!x86_legal_same_class_and_size(lhs_info, rhs_info)) {
            return x86_legal_fail(error, error_len,
                                  "x86 MIR compare %zu has incompatible operands",
                                  instr_index);
        }
        return true;
    }

    if (!x86_legal_same_class_and_size(def, lhs_info) ||
        !x86_legal_same_class_and_size(lhs_info, rhs_info)) {
        return x86_legal_fail(error, error_len,
                              "x86 MIR instruction %zu has incompatible binary operands",
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

    return x86_legal_fail(error, error_len,
                          "x86 MIR instruction %zu uses an illegal binary opcode/class pair",
                          instr_index);
}

static bool x86_legal_call(const anvil_mir_func_t *mir,
                           size_t instr_index,
                           const anvil_mir_instr_info_t *instr,
                           char *error,
                           size_t error_len)
{
    if (instr->def != ANVIL_MIR_NO_VREG) {
        const anvil_mir_vreg_info_t *def =
            x86_legal_vreg_info(mir, instr->def, instr_index,
                                error, error_len);
        if (!def) return false;
    }

    size_t arg_start = 0;
    if (!instr->symbol || !instr->symbol[0]) {
        if (instr->num_uses == 0) {
            return x86_legal_fail(error, error_len,
                                  "x86 MIR indirect call %zu requires a target register",
                                  instr_index);
        }
        anvil_mir_vreg_t target = anvil_mir_get_instr_use(mir, instr_index, 0);
        const anvil_mir_vreg_info_t *target_info =
            x86_legal_vreg_info(mir, target, instr_index, error, error_len);
        if (!target_info) return false;
        if (target_info->reg_class != ANVIL_MIR_REG_GPR ||
            target_info->size_bits != 32) {
            return x86_legal_fail(error, error_len,
                                  "x86 MIR indirect call %zu target must be a pointer register",
                                  instr_index);
        }
        arg_start = 1;
    }

    for (size_t u = arg_start; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, instr_index, u);
        if (!x86_legal_vreg_info(mir, use, instr_index, error, error_len)) {
            return false;
        }
    }

    return true;
}

static bool x86_legal_instr(const anvil_mir_func_t *mir,
                            size_t instr_index,
                            const anvil_mir_instr_info_t *instr,
                            char *error,
                            size_t error_len)
{
    if (instr->def != ANVIL_MIR_NO_VREG &&
        !x86_legal_vreg_info(mir, instr->def, instr_index,
                             error, error_len)) {
        return false;
    }
    for (size_t u = 0; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, instr_index, u);
        if (!x86_legal_vreg_info(mir, use, instr_index,
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
            return x86_legal_binary(mir, instr_index, instr,
                                    error, error_len);

        case ANVIL_MIR_OP_LOAD: {
            anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(mir, instr_index, 0);
            return x86_legal_pointer_operand(mir, ptr, instr_index,
                                             error, error_len);
        }

        case ANVIL_MIR_OP_STORE: {
            anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(mir, instr_index, 1);
            return x86_legal_pointer_operand(mir, ptr, instr_index,
                                             error, error_len);
        }

        case ANVIL_MIR_OP_SYMBOL_ADDR:
        case ANVIL_MIR_OP_FRAME_ADDR:
        case ANVIL_MIR_OP_DYN_ALLOCA: {
            const anvil_mir_vreg_info_t *def =
                x86_legal_vreg_info(mir, instr->def, instr_index,
                                    error, error_len);
            if (def && def->reg_class == ANVIL_MIR_REG_GPR &&
                def->size_bits == 32) {
                return true;
            }
            return x86_legal_fail(error, error_len,
                                  "x86 MIR instruction %zu must define a 32-bit pointer",
                                  instr_index);
        }

        case ANVIL_MIR_OP_INCOMING_STACK_ARG:
        case ANVIL_MIR_OP_CALL_STACK_ARG:
            if (instr->has_imm && instr->imm >= 0 && (instr->imm % 4) == 0) {
                return true;
            }
            return x86_legal_fail(error, error_len,
                                  "x86 MIR stack instruction %zu needs an aligned stack offset",
                                  instr_index);

        case ANVIL_MIR_OP_CALL:
            return x86_legal_call(mir, instr_index, instr,
                                  error, error_len);

        case ANVIL_MIR_OP_SELECT:
            return x86_legal_fail(error, error_len,
                                  "x86 MIR select %zu must be lowered to a branch",
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
        case ANVIL_MIR_OP_RET:
        case ANVIL_MIR_OP_BR:
        case ANVIL_MIR_OP_BR_COND:
        case ANVIL_MIR_OP_OTHER:
            return true;

        case ANVIL_MIR_OP_INVALID:
        default:
            break;
    }

    return x86_legal_fail(error, error_len,
                          "x86 MIR instruction %zu uses unsupported opcode",
                          instr_index);
}

bool anvil_x86_verify_mir_legal(const anvil_mir_func_t *mir,
                                char *error,
                                size_t error_len)
{
    if (error && error_len > 0) error[0] = '\0';
    if (!anvil_mir_verify(mir, error, error_len)) return false;

    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, i, &instr)) {
            return x86_legal_fail(error, error_len,
                                  "x86 MIR instruction %zu is not inspectable",
                                  i);
        }
        if (!x86_legal_instr(mir, i, &instr, error, error_len)) {
            return false;
        }
    }

    return true;
}

bool anvil_x86_regalloc_mir(anvil_mir_func_t *mir)
{
    if (!mir) return false;

    const anvil_x86_cc_desc_t *desc = anvil_x86_get_cc_desc(ANVIL_CC_CDECL);
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

    if (!anvil_x86_verify_mir_legal(mir, NULL, 0)) return false;
    if (!anvil_mir_coalesce_copies(mir)) return false;
    if (!anvil_x86_verify_mir_legal(mir, NULL, 0)) return false;
    if (!anvil_regalloc_linear_scan_classes(mir, configs, num_configs)) {
        return false;
    }
    if (!anvil_mir_materialize_spills(mir, scratch_configs,
                                      num_scratch_configs)) {
        return false;
    }
    return anvil_x86_verify_mir_legal(mir, NULL, 0);
}

typedef struct {
    const anvil_mir_func_t *mir;
    const anvil_x86_cc_desc_t *desc;
    const anvil_x86_plat_desc_t *plat;
    anvil_syntax_t syntax;
    anvil_strbuf_t code;
    int *spill_offsets;
    size_t num_spill_offsets;
    int *frame_slot_offsets;
    size_t num_frame_slot_offsets;
    int gpr_save_offsets[8];
    int scratch_a_off;
    int scratch_b_off;
    int outgoing_size;
    int frame_size;
    int ret_pop_bytes;
    bool fp_returns;
    bool emitted_fneg_mask;
    bool emitted_fabs_mask;
    bool failed;
} x86_mir_emit_t;

static int x86_align_int(int value, int align)
{
    return (value + align - 1) & ~(align - 1);
}

static int x86_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0) return 4;
    int size = (int)((size_bits + 7) / 8);
    if (size <= 0) return 4;
    if (size > 8) return 8;
    return size;
}

static int x86_slot_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0) return 4;
    int size = (int)((size_bits + 7) / 8);
    return size > 0 ? size : 4;
}

static const anvil_mir_vreg_info_t *x86_vreg_info_checked(
    x86_mir_emit_t *emit,
    anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(emit->mir, vreg);
    if (!info) emit->failed = true;
    return info;
}

static int x86_phys_of(x86_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *assignment =
        anvil_mir_get_assignment(emit->mir, vreg);
    if (!assignment || assignment->spilled || assignment->phys_reg < 0) {
        emit->failed = true;
        return -1;
    }
    return assignment->phys_reg;
}

static const char *x86_gpr_name(int phys_reg, int size)
{
    if (phys_reg < 0 || phys_reg >= 8) return "?";
    switch (size) {
        case 1: return x86_reg8_names[phys_reg];
        case 2: return x86_reg16_names[phys_reg];
        default: return x86_reg32_names[phys_reg];
    }
}

static const char *x86_reg_name(x86_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = x86_vreg_info_checked(emit, vreg);
    int phys = x86_phys_of(emit, vreg);
    if (!info || emit->failed) return "?";

    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        if (phys < 0 || phys >= 8) {
            emit->failed = true;
            return "?";
        }
        return x86_xmm_names[phys];
    }

    int size = x86_size_bytes(info->size_bits);
    return x86_gpr_name(phys, size);
}

static bool x86_emit_label(x86_mir_emit_t *emit, anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info)) return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s:\n",
                         anvil_mir_func_name(emit->mir), name);
    return true;
}

static bool x86_emit_branch_target(x86_mir_emit_t *emit,
                                   anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info)) return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s",
                         anvil_mir_func_name(emit->mir), name);
    return true;
}

static const char *x86_symbol_prefix(const x86_mir_emit_t *emit)
{
    return emit && emit->plat ? emit->plat->sym_prefix : "";
}

static bool x86_symbol_is_local(const char *symbol)
{
    return symbol && symbol[0] == '.';
}

static const char *x86_symbol_ref_prefix(const x86_mir_emit_t *emit,
                                         const char *symbol)
{
    return x86_symbol_is_local(symbol) ? "" : x86_symbol_prefix(emit);
}

static const char *x86_setcc(anvil_mir_opcode_t op)
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

static const char *x86_size_suffix(int size)
{
    switch (size) {
        case 1: return "b";
        case 2: return "w";
        default: return "l";
    }
}

static bool x86_scan_outgoing_stack_args(x86_mir_emit_t *emit)
{
    int outgoing_size = 0;

    for (size_t i = 0; i < anvil_mir_num_instrs(emit->mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(emit->mir, i, &info)) return false;
        if (info.op != ANVIL_MIR_OP_CALL_STACK_ARG) continue;
        if (!info.has_imm || info.imm < 0 || info.num_uses != 1) return false;
        if (info.imm > INT32_MAX - 4) return false;

        int end = (int)info.imm + 4;
        if (end > outgoing_size) outgoing_size = end;
    }

    emit->outgoing_size = x86_align_int(outgoing_size, 16);
    return true;
}

static bool x86_func_returns_fp(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info)) return false;
        if (info.op != ANVIL_MIR_OP_RET || info.num_uses == 0) continue;
        anvil_mir_vreg_t ret = anvil_mir_get_instr_use(mir, i, 0);
        const anvil_mir_vreg_info_t *rinfo = anvil_mir_get_vreg_info(mir, ret);
        if (rinfo && rinfo->reg_class == ANVIL_MIR_REG_FPR) return true;
    }
    return false;
}

static bool x86_prepare_frame(x86_mir_emit_t *emit)
{
    for (size_t i = 0; i < 8; i++) {
        emit->gpr_save_offsets[i] = -1;
    }

    if (!x86_scan_outgoing_stack_args(emit)) return false;
    emit->fp_returns = x86_func_returns_fp(emit->mir);

    int offset = 0;
    static const int callee_saved[] = { X86_EBX, X86_ESI, X86_EDI };
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
            offset += 4;
            emit->gpr_save_offsets[reg] = offset;
        }
    }

    offset += 4;
    emit->scratch_a_off = offset;
    offset += 4;
    emit->scratch_b_off = offset;

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
        int align = slot.align_bytes ? slot.align_bytes : 4;
        if (align > 16) align = 16;
        offset = x86_align_int(offset, align);
        offset += x86_slot_size_bytes(slot.size_bits);
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
        offset += x86_align_int(x86_slot_size_bytes(slot.size_bits), 4);
        emit->spill_offsets[i] = offset;
    }

    offset += emit->outgoing_size;
    emit->frame_size = x86_align_int(offset, 16);
    return true;
}

static void x86_emit_func_header(x86_mir_emit_t *emit)
{
    const char *name = anvil_mir_func_name(emit->mir);
    const char *prefix = x86_symbol_prefix(emit);
    if (emit->plat->is_macho) {
        anvil_strbuf_append(&emit->code,
                            "\t.section __TEXT,__text,regular,pure_instructions\n");
        anvil_strbuf_appendf(&emit->code, "\t.globl %s%s\n", prefix, name);
        anvil_strbuf_append(&emit->code, "\t.p2align 4\n");
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    } else if (emit->plat->is_coff) {
        anvil_strbuf_append(&emit->code, "\t.text\n");
        anvil_strbuf_appendf(&emit->code, "\t.globl %s%s\n", prefix, name);
        anvil_strbuf_appendf(&emit->code, "\t.def %s%s; .scl 2; .type 32; .endef\n",
                             prefix, name);
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    } else {
        anvil_strbuf_append(&emit->code, "\t.text\n");
        anvil_strbuf_appendf(&emit->code, "\t.globl %s%s\n", prefix, name);
        anvil_strbuf_appendf(&emit->code, "\t.type %s%s, @function\n",
                             prefix, name);
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    }
}

static void x86_emit_prologue(x86_mir_emit_t *emit)
{
    anvil_strbuf_append(&emit->code, "\tpushl %ebp\n");
    anvil_strbuf_append(&emit->code, "\tmovl %esp, %ebp\n");
    if (emit->frame_size > 0) {
        anvil_strbuf_appendf(&emit->code, "\tsubl $%d, %%esp\n", emit->frame_size);
    }

    for (int reg = 0; reg < 8; reg++) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, -%d(%%ebp)\n",
                                 x86_reg32_names[reg],
                                 emit->gpr_save_offsets[reg]);
        }
    }
}

static void x86_emit_epilogue(x86_mir_emit_t *emit)
{
    for (int reg = 7; reg >= 0; reg--) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->code, "\tmovl -%d(%%ebp), %%%s\n",
                                 emit->gpr_save_offsets[reg],
                                 x86_reg32_names[reg]);
        }
    }
    anvil_strbuf_append(&emit->code, "\tmovl %ebp, %esp\n");
    anvil_strbuf_append(&emit->code, "\tpopl %ebp\n");
    if (emit->desc->callee_cleans_stack && emit->ret_pop_bytes > 0) {
        anvil_strbuf_appendf(&emit->code, "\tret $%d\n", emit->ret_pop_bytes);
    } else {
        anvil_strbuf_append(&emit->code, "\tret\n");
    }
}

static const char *x86_fp_mov_op(int size)
{
    return size <= 4 ? "movss" : "movsd";
}

static void x86_emit_copy(x86_mir_emit_t *emit,
                          anvil_mir_vreg_t dst,
                          anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *dst_info = x86_vreg_info_checked(emit, dst);
    const anvil_mir_vreg_info_t *src_info = x86_vreg_info_checked(emit, src);
    int dst_phys = x86_phys_of(emit, dst);
    int src_phys = x86_phys_of(emit, src);
    if (!dst_info || !src_info || emit->failed) return;

    if (dst_info->reg_class == ANVIL_MIR_REG_FPR &&
        src_info->reg_class == ANVIL_MIR_REG_FPR) {
        if (dst_phys == src_phys) return;
        int size = x86_size_bytes(dst_info->size_bits);
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n",
                             x86_fp_mov_op(size),
                             x86_xmm_names[src_phys], x86_xmm_names[dst_phys]);
    } else if (dst_info->reg_class == ANVIL_MIR_REG_GPR &&
               src_info->reg_class == ANVIL_MIR_REG_GPR) {
        if (dst_phys == src_phys) return;
        int size = x86_size_bytes(dst_info->size_bits);
        if (size < 4) size = 4;
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n",
                             x86_size_suffix(size),
                             x86_gpr_name(src_phys, size),
                             x86_gpr_name(dst_phys, size));
    } else if (dst_info->reg_class == ANVIL_MIR_REG_FPR &&
               src_info->reg_class == ANVIL_MIR_REG_GPR) {
        anvil_strbuf_appendf(&emit->code, "\tmovd %%%s, %%%s\n",
                             x86_gpr_name(src_phys, 4), x86_xmm_names[dst_phys]);
    } else if (dst_info->reg_class == ANVIL_MIR_REG_GPR &&
               src_info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\tmovd %%%s, %%%s\n",
                             x86_xmm_names[src_phys], x86_gpr_name(dst_phys, 4));
    } else {
        emit->failed = true;
    }
}

static void x86_emit_mov(x86_mir_emit_t *emit,
                         const anvil_mir_instr_info_t *info)
{
    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    if (!def_info) return;
    int dst_phys = x86_phys_of(emit, info->def);
    if (emit->failed) return;

    int64_t imm = info->has_imm ? info->imm : 0;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x86_size_bytes(def_info->size_bits);
        const char *dst = x86_xmm_names[dst_phys];
        if (size <= 4) {
            anvil_strbuf_appendf(&emit->code, "\tmovl $%u, -%d(%%ebp)\n",
                                 (uint32_t)imm, emit->scratch_a_off);
            anvil_strbuf_appendf(&emit->code, "\tmovss -%d(%%ebp), %%%s\n",
                                 emit->scratch_a_off, dst);
        } else {
            uint64_t bits = (uint64_t)imm;
            anvil_strbuf_appendf(&emit->code, "\tmovl $%u, -%d(%%ebp)\n",
                                 (uint32_t)(bits & 0xffffffffu),
                                 emit->scratch_b_off);
            anvil_strbuf_appendf(&emit->code, "\tmovl $%u, -%d(%%ebp)\n",
                                 (uint32_t)(bits >> 32), emit->scratch_b_off - 4);
            anvil_strbuf_appendf(&emit->code, "\tmovsd -%d(%%ebp), %%%s\n",
                                 emit->scratch_b_off, dst);
        }
        return;
    }

    int size = x86_size_bytes(def_info->size_bits);
    anvil_strbuf_appendf(&emit->code, "\tmovl $%lld, %%%s\n",
                         (long long)(int64_t)(int32_t)imm,
                         x86_gpr_name(dst_phys, size < 4 ? 4 : size));
}

static void x86_emit_gpr_simple_binary(x86_mir_emit_t *emit,
                                       const char *mnemonic, const char *suf,
                                       int dst_phys, int a_phys, int b_phys,
                                       int size, bool commutative)
{
    const char *dst = x86_gpr_name(dst_phys, size);
    const char *a = x86_gpr_name(a_phys, size);
    const char *b = x86_gpr_name(b_phys, size);

    if (dst_phys == b_phys && !commutative) {
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, -%d(%%ebp)\n",
                             suf, b, emit->scratch_b_off);
        if (dst_phys != a_phys) {
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n",
                                 suf, a, dst);
        }
        anvil_strbuf_appendf(&emit->code, "\t%s%s -%d(%%ebp), %%%s\n",
                             mnemonic, suf, emit->scratch_b_off, dst);
        return;
    }

    if (dst_phys == b_phys && commutative) {
        anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n",
                             mnemonic, suf, a, dst);
        return;
    }

    if (dst_phys != a_phys) {
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, dst);
    }
    anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n",
                         mnemonic, suf, b, dst);
}

static void x86_emit_gpr_binary(x86_mir_emit_t *emit,
                                const anvil_mir_instr_info_t *info,
                                int dst_phys, int a_phys, int b_phys, int size)
{
    int ssize = size < 4 ? 4 : size;
    const char *ssuf = x86_size_suffix(ssize);
    if (emit->failed) return;

    switch (info->op) {
        case ANVIL_MIR_OP_ADD:
            x86_emit_gpr_simple_binary(emit, "add", ssuf, dst_phys, a_phys,
                                       b_phys, ssize, true);
            break;
        case ANVIL_MIR_OP_SUB:
            x86_emit_gpr_simple_binary(emit, "sub", ssuf, dst_phys, a_phys,
                                       b_phys, ssize, false);
            break;
        case ANVIL_MIR_OP_AND:
            x86_emit_gpr_simple_binary(emit, "and", ssuf, dst_phys, a_phys,
                                       b_phys, ssize, true);
            break;
        case ANVIL_MIR_OP_OR:
            x86_emit_gpr_simple_binary(emit, "or", ssuf, dst_phys, a_phys,
                                       b_phys, ssize, true);
            break;
        case ANVIL_MIR_OP_XOR:
            x86_emit_gpr_simple_binary(emit, "xor", ssuf, dst_phys, a_phys,
                                       b_phys, ssize, true);
            break;
        case ANVIL_MIR_OP_MUL: {
            const char *b = x86_gpr_name(b_phys, size < 4 ? 4 : size);
            int wsize = size < 4 ? 4 : size;
            const char *wsuf = x86_size_suffix(wsize);
            const char *wdst = x86_gpr_name(dst_phys, wsize);
            const char *wa = x86_gpr_name(a_phys, wsize);
            if (dst_phys == b_phys) {
                anvil_strbuf_appendf(&emit->code, "\timul%s %%%s, %%%s\n",
                                     wsuf, wa, wdst);
            } else {
                if (dst_phys != a_phys) {
                    anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n",
                                         wsuf, wa, wdst);
                }
                anvil_strbuf_appendf(&emit->code, "\timul%s %%%s, %%%s\n",
                                     wsuf, b, wdst);
            }
            break;
        }
        case ANVIL_MIR_OP_SHL:
        case ANVIL_MIR_OP_SHR:
        case ANVIL_MIR_OP_SAR: {
            const char *sh = info->op == ANVIL_MIR_OP_SHL ? "shl"
                           : info->op == ANVIL_MIR_OP_SHR ? "shr" : "sar";
            const char *suf = x86_size_suffix(size);
            const char *a = x86_gpr_name(a_phys, size);
            const char *dst = x86_gpr_name(dst_phys, size);
            const char *b = x86_gpr_name(b_phys, 4);
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, -%d(%%ebp)\n",
                                 b, emit->scratch_b_off);
            anvil_strbuf_appendf(&emit->code, "\tmovl -%d(%%ebp), %%ecx\n",
                                 emit->scratch_b_off);
            if (size == 1) {
                const char *scratch8 = x86_reg8_names[X86_EAX];
                anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%eax\n",
                                     x86_gpr_name(a_phys, 4));
                anvil_strbuf_appendf(&emit->code, "\t%sb %%cl, %%%s\n",
                                     sh, scratch8);
                anvil_strbuf_appendf(&emit->code, "\tmovzbl %%%s, %%%s\n",
                                     scratch8, x86_gpr_name(dst_phys, 4));
            } else {
                anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, -%d(%%ebp)\n",
                                     suf, a, emit->scratch_a_off);
                anvil_strbuf_appendf(&emit->code, "\tmov%s -%d(%%ebp), %%%s\n",
                                     suf, emit->scratch_a_off, dst);
                anvil_strbuf_appendf(&emit->code, "\t%s%s %%cl, %%%s\n",
                                     sh, suf, dst);
            }
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
            int wsize = size < 4 ? 4 : size;
            const char *wsuf = x86_size_suffix(wsize);
            const char *wa = x86_gpr_name(a_phys, wsize);
            const char *wb = x86_gpr_name(b_phys, wsize);
            const char *wdst = x86_gpr_name(dst_phys, wsize);
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, -%d(%%ebp)\n",
                                 wsuf, wb, emit->scratch_b_off);
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, -%d(%%ebp)\n",
                                 wsuf, wa, emit->scratch_a_off);
            anvil_strbuf_appendf(&emit->code, "\tmov%s -%d(%%ebp), %%eax\n",
                                 wsuf, emit->scratch_a_off);
            if (is_signed) {
                anvil_strbuf_append(&emit->code, "\tcltd\n");
                anvil_strbuf_appendf(&emit->code, "\tidiv%s -%d(%%ebp)\n",
                                     wsuf, emit->scratch_b_off);
            } else {
                anvil_strbuf_append(&emit->code, "\txorl %edx, %edx\n");
                anvil_strbuf_appendf(&emit->code, "\tdiv%s -%d(%%ebp)\n",
                                     wsuf, emit->scratch_b_off);
            }
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n",
                                 wsuf, is_mod ? "edx" : "eax", wdst);
            break;
        }
        default:
            emit->failed = true;
            break;
    }
}

static void x86_emit_binary(x86_mir_emit_t *emit,
                            size_t instr_index,
                            const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    if (!def_info) return;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x86_size_bytes(def_info->size_bits);
        const char *dst = x86_reg_name(emit, info->def);
        const char *a = x86_reg_name(emit, lhs);
        const char *b = x86_reg_name(emit, rhs);
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
                             x86_fp_mov_op(size), a, dst);
        anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n",
                             op, sfx, b, dst);
        return;
    }

    int size = x86_size_bytes(def_info->size_bits);
    int dst_phys = x86_phys_of(emit, info->def);
    int a_phys = x86_phys_of(emit, lhs);
    int b_phys = x86_phys_of(emit, rhs);
    if (emit->failed) return;
    x86_emit_gpr_binary(emit, info, dst_phys, a_phys, b_phys, size);
}

static void x86_emit_cmp(x86_mir_emit_t *emit,
                         size_t instr_index,
                         const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *lhs_info = x86_vreg_info_checked(emit, lhs);
    int dst_phys = x86_phys_of(emit, info->def);
    if (!lhs_info || emit->failed) return;

    const char *dst32 = x86_gpr_name(dst_phys, 4);
    const char *scratch8 = x86_reg8_names[X86_EAX];

    if (lhs_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x86_size_bytes(lhs_info->size_bits);
        const char *a = x86_reg_name(emit, lhs);
        const char *b = x86_reg_name(emit, rhs);
        if (emit->failed) return;
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n",
                             size <= 4 ? "ucomiss" : "ucomisd", b, a);
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s\n",
                             x86_setcc(info->op), scratch8);
        anvil_strbuf_appendf(&emit->code, "\tmovzbl %%%s, %%%s\n", scratch8, dst32);
        return;
    }

    int size = x86_size_bytes(lhs_info->size_bits);
    int a_phys = x86_phys_of(emit, lhs);
    int b_phys = x86_phys_of(emit, rhs);
    if (emit->failed) return;
    if (size == 1) {
        anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%eax\n",
                             x86_gpr_name(a_phys, 4));
        anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%edx\n",
                             x86_gpr_name(b_phys, 4));
        anvil_strbuf_appendf(&emit->code, "\tcmpb %%%s, %%%s\n",
                             x86_reg8_names[X86_EDX], x86_reg8_names[X86_EAX]);
    } else {
        const char *suf = x86_size_suffix(size);
        const char *a = x86_gpr_name(a_phys, size);
        const char *b = x86_gpr_name(b_phys, size);
        anvil_strbuf_appendf(&emit->code, "\tcmp%s %%%s, %%%s\n", suf, b, a);
    }
    anvil_strbuf_appendf(&emit->code, "\t%s %%%s\n", x86_setcc(info->op), scratch8);
    anvil_strbuf_appendf(&emit->code, "\tmovzbl %%%s, %%%s\n", scratch8, dst32);
}

static void x86_emit_unary(x86_mir_emit_t *emit,
                           size_t instr_index,
                           const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    if (!def_info || emit->failed) return;

    if (info->op == ANVIL_MIR_OP_NEG) {
        if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
            int size = x86_size_bytes(def_info->size_bits);
            const char *dst = x86_reg_name(emit, info->def);
            const char *s = x86_reg_name(emit, src);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n",
                                 x86_fp_mov_op(size), s, dst);
            anvil_strbuf_appendf(&emit->code, "\t%s .Lx86_fneg_mask%s, %%%s\n",
                                 size <= 4 ? "xorps" : "xorpd",
                                 size <= 4 ? "32" : "64", dst);
            emit->emitted_fneg_mask = true;
        } else {
            int size = x86_size_bytes(def_info->size_bits);
            if (size < 4) size = 4;
            const char *suf = x86_size_suffix(size);
            const char *dst = x86_gpr_name(x86_phys_of(emit, info->def), size);
            const char *s = x86_gpr_name(x86_phys_of(emit, src), size);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, s, dst);
            anvil_strbuf_appendf(&emit->code, "\tneg%s %%%s\n", suf, dst);
        }
    } else if (info->op == ANVIL_MIR_OP_FABS &&
               def_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x86_size_bytes(def_info->size_bits);
        const char *dst = x86_reg_name(emit, info->def);
        const char *s = x86_reg_name(emit, src);
        if (emit->failed) return;
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n",
                             x86_fp_mov_op(size), s, dst);
        anvil_strbuf_appendf(&emit->code, "\t%s .Lx86_fabs_mask%s, %%%s\n",
                             size <= 4 ? "andps" : "andpd",
                             size <= 4 ? "32" : "64", dst);
        emit->emitted_fabs_mask = true;
    } else if (info->op == ANVIL_MIR_OP_NOT &&
               def_info->reg_class == ANVIL_MIR_REG_GPR) {
        int size = x86_size_bytes(def_info->size_bits);
        if (size < 4) size = 4;
        const char *suf = x86_size_suffix(size);
        const char *dst = x86_gpr_name(x86_phys_of(emit, info->def), size);
        const char *s = x86_gpr_name(x86_phys_of(emit, src), size);
        if (emit->failed) return;
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, s, dst);
        anvil_strbuf_appendf(&emit->code, "\tnot%s %%%s\n", suf, dst);
    } else {
        emit->failed = true;
    }
}

static void x86_emit_gpr_extend(x86_mir_emit_t *emit,
                                size_t instr_index,
                                const anvil_mir_instr_info_t *info,
                                bool sign_extend)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = x86_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = x86_vreg_info_checked(emit, src);
    int dst_phys = x86_phys_of(emit, info->def);
    int src_phys = x86_phys_of(emit, src);
    if (!dst_info || !src_info || emit->failed) return;
    if (dst_info->reg_class != ANVIL_MIR_REG_GPR ||
        src_info->reg_class != ANVIL_MIR_REG_GPR) {
        emit->failed = true;
        return;
    }

    int src_size = x86_size_bytes(src_info->size_bits);
    int dst_size = x86_size_bytes(dst_info->size_bits);
    if (dst_size < src_size) dst_size = src_size;
    if (dst_size < 4) dst_size = 4;

    if (src_size >= 4) {
        anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n",
                             x86_gpr_name(src_phys, 4),
                             x86_gpr_name(dst_phys, 4));
        return;
    }

    const char *zs = sign_extend ? "movs" : "movz";
    if (src_size == 1 && !x86_reg_has_byte(src_phys)) {
        anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%eax\n",
                             x86_gpr_name(src_phys, 4));
        anvil_strbuf_appendf(&emit->code, "\t%sbl %%%s, %%%s\n",
                             zs, x86_reg8_names[X86_EAX],
                             x86_gpr_name(dst_phys, 4));
        return;
    }
    const char *suffix = src_size == 1 ? "bl" : "wl";
    anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n",
                         zs, suffix, x86_gpr_name(src_phys, src_size),
                         x86_gpr_name(dst_phys, 4));
}

static void x86_emit_cast(x86_mir_emit_t *emit,
                          size_t instr_index,
                          const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = x86_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = x86_vreg_info_checked(emit, src);
    if (!dst_info || !src_info) return;

    switch (info->op) {
        case ANVIL_MIR_OP_ZEXT:
            x86_emit_gpr_extend(emit, instr_index, info, false);
            break;
        case ANVIL_MIR_OP_SEXT:
            x86_emit_gpr_extend(emit, instr_index, info, true);
            break;
        case ANVIL_MIR_OP_TRUNC:
        case ANVIL_MIR_OP_BITCAST:
            x86_emit_copy(emit, info->def, src);
            break;
        case ANVIL_MIR_OP_SITOFP: {
            if (dst_info->reg_class != ANVIL_MIR_REG_FPR ||
                src_info->reg_class != ANVIL_MIR_REG_GPR) {
                emit->failed = true;
                return;
            }
            int dst_size = x86_size_bytes(dst_info->size_bits);
            const char *dst = x86_reg_name(emit, info->def);
            const char *s = x86_gpr_name(x86_phys_of(emit, src), 4);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\tcvtsi2s%sl %%%s, %%%s\n",
                                 dst_size <= 4 ? "s" : "d", s, dst);
            break;
        }
        case ANVIL_MIR_OP_UITOFP: {
            if (dst_info->reg_class != ANVIL_MIR_REG_FPR ||
                src_info->reg_class != ANVIL_MIR_REG_GPR) {
                emit->failed = true;
                return;
            }
            int dst_size = x86_size_bytes(dst_info->size_bits);
            const char *dst = x86_reg_name(emit, info->def);
            const char *s = x86_gpr_name(x86_phys_of(emit, src), 4);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, -%d(%%ebp)\n",
                                 s, emit->scratch_b_off);
            anvil_strbuf_append(&emit->code, "\tmovl $0, -");
            anvil_strbuf_appendf(&emit->code, "%d(%%ebp)\n",
                                 emit->scratch_b_off - 4);
            anvil_strbuf_appendf(&emit->code,
                                 "\tcvtsi2s%sq -%d(%%ebp), %%%s\n",
                                 dst_size <= 4 ? "s" : "d",
                                 emit->scratch_b_off, dst);
            break;
        }
        case ANVIL_MIR_OP_FPTOSI:
        case ANVIL_MIR_OP_FPTOUI: {
            if (dst_info->reg_class != ANVIL_MIR_REG_GPR ||
                src_info->reg_class != ANVIL_MIR_REG_FPR) {
                emit->failed = true;
                return;
            }
            int src_size = x86_size_bytes(src_info->size_bits);
            const char *s = x86_reg_name(emit, src);
            const char *dst = x86_gpr_name(x86_phys_of(emit, info->def), 4);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\tcvtts%s2si %%%s, %%%s\n",
                                 src_size <= 4 ? "s" : "d", s, dst);
            break;
        }
        case ANVIL_MIR_OP_FPEXT: {
            const char *dst = x86_reg_name(emit, info->def);
            const char *s = x86_reg_name(emit, src);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\tcvtss2sd %%%s, %%%s\n", s, dst);
            break;
        }
        case ANVIL_MIR_OP_FPTRUNC: {
            const char *dst = x86_reg_name(emit, info->def);
            const char *s = x86_reg_name(emit, src);
            if (emit->failed) return;
            anvil_strbuf_appendf(&emit->code, "\tcvtsd2ss %%%s, %%%s\n", s, dst);
            break;
        }
        default:
            emit->failed = true;
            break;
    }
}

static void x86_emit_frame_addr(x86_mir_emit_t *emit,
                                const anvil_mir_instr_info_t *info)
{
    if (info->frame_slot < 0 ||
        (size_t)info->frame_slot >= emit->num_frame_slot_offsets) {
        emit->failed = true;
        return;
    }

    int dst_phys = x86_phys_of(emit, info->def);
    if (emit->failed) return;

    int offset = emit->frame_slot_offsets[info->frame_slot];
    anvil_strbuf_appendf(&emit->code, "\tleal -%d(%%ebp), %%%s\n",
                         offset, x86_gpr_name(dst_phys, 4));
}

static void x86_emit_dyn_alloca(x86_mir_emit_t *emit,
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

    const char *count_reg = x86_gpr_name(x86_phys_of(emit, count), 4);
    int dst_phys = x86_phys_of(emit, info->def);
    if (emit->failed) return;

    anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, -%d(%%ebp)\n",
                         count_reg, emit->scratch_a_off);
    anvil_strbuf_appendf(&emit->code, "\tmovl -%d(%%ebp), %%eax\n",
                         emit->scratch_a_off);
    if (info->imm != 1) {
        anvil_strbuf_appendf(&emit->code, "\timull $%lld, %%eax, %%eax\n",
                             (long long)info->imm);
    }
    anvil_strbuf_append(&emit->code, "\taddl $15, %eax\n");
    anvil_strbuf_append(&emit->code, "\tandl $-16, %eax\n");
    anvil_strbuf_append(&emit->code, "\tsubl %eax, %esp\n");
    anvil_strbuf_appendf(&emit->code, "\tmovl %%esp, %%%s\n",
                         x86_gpr_name(dst_phys, 4));
}

static void x86_emit_symbol_addr(x86_mir_emit_t *emit,
                                 const anvil_mir_instr_info_t *info)
{
    if (!info->symbol || !info->symbol[0]) {
        emit->failed = true;
        return;
    }

    int dst_phys = x86_phys_of(emit, info->def);
    if (emit->failed) return;

    const char *prefix = x86_symbol_ref_prefix(emit, info->symbol);
    const char *dst = x86_gpr_name(dst_phys, 4);
    anvil_strbuf_appendf(&emit->code, "\tmovl $%s%s, %%%s\n",
                         prefix, info->symbol, dst);
}

static void x86_emit_load(x86_mir_emit_t *emit,
                          size_t instr_index,
                          const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    int def_phys = x86_phys_of(emit, info->def);
    const char *base = x86_gpr_name(x86_phys_of(emit, ptr), 4);
    if (!def_info || emit->failed) return;

    int size = x86_size_bytes(def_info->size_bits);
    int64_t offset = info->has_imm ? info->imm : 0;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        const char *op = size <= 4 ? "movss" : "movsd";
        const char *dst = x86_xmm_names[def_phys];
        if (offset == 0) {
            anvil_strbuf_appendf(&emit->code, "\t%s (%%%s), %%%s\n", op, base, dst);
        } else {
            anvil_strbuf_appendf(&emit->code, "\t%s %lld(%%%s), %%%s\n",
                                 op, (long long)offset, base, dst);
        }
        return;
    }

    const char *op;
    const char *dst;
    if (def_info->is_signed && size == 1) { op = "movsbl"; dst = x86_gpr_name(def_phys, 4); }
    else if (def_info->is_signed && size == 2) { op = "movswl"; dst = x86_gpr_name(def_phys, 4); }
    else if (!def_info->is_signed && size == 1) { op = "movzbl"; dst = x86_gpr_name(def_phys, 4); }
    else if (!def_info->is_signed && size == 2) { op = "movzwl"; dst = x86_gpr_name(def_phys, 4); }
    else { op = "movl"; dst = x86_gpr_name(def_phys, 4); }

    if (offset == 0) {
        anvil_strbuf_appendf(&emit->code, "\t%s (%%%s), %%%s\n", op, base, dst);
    } else {
        anvil_strbuf_appendf(&emit->code, "\t%s %lld(%%%s), %%%s\n",
                             op, (long long)offset, base, dst);
    }
}

static void x86_emit_store(x86_mir_emit_t *emit, size_t instr_index)
{
    anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (value == ANVIL_MIR_NO_VREG || ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    const anvil_mir_vreg_info_t *value_info = x86_vreg_info_checked(emit, value);
    int ptr_phys = x86_phys_of(emit, ptr);
    const char *base = x86_gpr_name(ptr_phys, 4);
    if (!value_info || emit->failed) return;

    int size = x86_size_bytes(value_info->size_bits);
    const char *op;
    const char *src;
    if (value_info->reg_class == ANVIL_MIR_REG_FPR) {
        op = size <= 4 ? "movss" : "movsd";
        src = x86_reg_name(emit, value);
    } else {
        int value_phys = x86_phys_of(emit, value);
        op = size == 1 ? "movb" : size == 2 ? "movw" : "movl";
        if (size == 1 && !x86_reg_has_byte(value_phys)) {
            int scratch = ptr_phys == X86_EAX ? X86_ECX : X86_EAX;
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n",
                                 x86_gpr_name(value_phys, 4),
                                 x86_gpr_name(scratch, 4));
            src = x86_reg8_names[scratch];
        } else {
            src = x86_gpr_name(value_phys, size);
        }
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

static void x86_emit_incoming_stack_arg(x86_mir_emit_t *emit,
                                        const anvil_mir_instr_info_t *info)
{
    if (!info->has_imm || info->imm < 0 || info->imm > INT32_MAX - 32) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    int def_phys = x86_phys_of(emit, info->def);
    if (!def_info || emit->failed) return;

    int size = x86_size_bytes(def_info->size_bits);
    int frame_offset = 8 + (int)info->imm;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        const char *op = size <= 4 ? "movss" : "movsd";
        anvil_strbuf_appendf(&emit->code, "\t%s %d(%%ebp), %%%s\n",
                             op, frame_offset, x86_xmm_names[def_phys]);
        return;
    }

    const char *op;
    if (def_info->is_signed && size == 1) op = "movsbl";
    else if (def_info->is_signed && size == 2) op = "movswl";
    else if (!def_info->is_signed && size == 1) op = "movzbl";
    else if (!def_info->is_signed && size == 2) op = "movzwl";
    else op = "movl";
    anvil_strbuf_appendf(&emit->code, "\t%s %d(%%ebp), %%%s\n",
                         op, frame_offset, x86_gpr_name(def_phys, 4));
}

static void x86_emit_call_stack_arg(x86_mir_emit_t *emit,
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

    const anvil_mir_vreg_info_t *value_info = x86_vreg_info_checked(emit, value);
    if (!value_info || emit->failed) return;

    int size = x86_size_bytes(value_info->size_bits);
    int slot = (int)info->imm;
    if (value_info->reg_class == ANVIL_MIR_REG_FPR) {
        const char *op = size <= 4 ? "movss" : "movsd";
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %d(%%esp)\n",
                             op, x86_reg_name(emit, value), slot);
        return;
    }
    const char *src = x86_gpr_name(x86_phys_of(emit, value), 4);
    if (emit->failed) return;
    anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %d(%%esp)\n", src, slot);
}

static int x86_call_stack_bytes(x86_mir_emit_t *emit, size_t call_index)
{
    int max_end = 0;
    size_t block = 0;
    anvil_mir_instr_info_t call_info;
    if (!anvil_mir_get_instr_info(emit->mir, call_index, &call_info)) return 0;
    block = call_info.block;

    for (size_t i = 0; i < anvil_mir_num_instrs(emit->mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(emit->mir, i, &info)) return 0;
        if (info.op != ANVIL_MIR_OP_CALL_STACK_ARG) continue;
        if (info.block != block) continue;
        if (!info.has_imm) continue;
        int end = (int)info.imm + 4;
        if (end > max_end) max_end = end;
    }
    return max_end;
}

static void x86_emit_call(x86_mir_emit_t *emit,
                          size_t instr_index,
                          const anvil_mir_instr_info_t *info)
{
    bool direct = info->symbol && info->symbol[0];
    size_t arg_start = direct ? 0 : 1;

    for (size_t u = arg_start; u < info->num_uses; u++) {
        size_t idx = u - arg_start;
        if ((int)idx >= emit->desc->num_reg_int_args) break;
        anvil_mir_vreg_t arg = anvil_mir_get_instr_use(emit->mir, instr_index, u);
        int arg_phys = x86_phys_of(emit, arg);
        int reg = emit->desc->reg_int_args[idx];
        if (emit->failed) return;
        if (arg_phys != reg) {
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n",
                                 x86_gpr_name(arg_phys, 4),
                                 x86_reg32_names[reg]);
        }
    }

    if (direct) {
        const char *prefix = x86_symbol_ref_prefix(emit, info->symbol);
        char decorated[256];
        if (emit->plat->is_coff && emit->desc->decor == X86_DECOR_STDCALL) {
            snprintf(decorated, sizeof(decorated), "%s%s@%d",
                     prefix, info->symbol, x86_call_stack_bytes(emit, instr_index));
            anvil_strbuf_appendf(&emit->code, "\tcall %s\n", decorated);
        } else if (emit->plat->is_coff &&
                   emit->desc->decor == X86_DECOR_FASTCALL) {
            snprintf(decorated, sizeof(decorated), "@%s@%d",
                     info->symbol, x86_call_stack_bytes(emit, instr_index));
            anvil_strbuf_appendf(&emit->code, "\tcall %s\n", decorated);
        } else {
            anvil_strbuf_appendf(&emit->code, "\tcall %s%s\n", prefix, info->symbol);
        }
    } else {
        anvil_mir_vreg_t target =
            anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        if (target == ANVIL_MIR_NO_VREG) {
            emit->failed = true;
            return;
        }
        const char *target_reg = x86_gpr_name(x86_phys_of(emit, target), 4);
        if (emit->failed) return;
        anvil_strbuf_appendf(&emit->code, "\tcall *%%%s\n", target_reg);
    }

    if (info->def != ANVIL_MIR_NO_VREG) {
        int def_phys = x86_phys_of(emit, info->def);
        if (emit->failed) return;
        if (def_phys != emit->desc->int_ret_reg) {
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n",
                                 x86_reg32_names[emit->desc->int_ret_reg],
                                 x86_gpr_name(def_phys, 4));
        }
    }
}

static void x86_emit_spill_load(x86_mir_emit_t *emit,
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

    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    int def_phys = x86_phys_of(emit, info->def);
    if (!def_info || emit->failed) return;

    int offset = emit->spill_offsets[info->spill_slot];
    int size = x86_size_bytes(slot.size_bits);
    if (slot.reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\t%s -%d(%%ebp), %%%s\n",
                             x86_fp_mov_op(size), offset,
                             x86_xmm_names[def_phys]);
    } else {
        if (size < 4) size = 4;
        anvil_strbuf_appendf(&emit->code, "\tmov%s -%d(%%ebp), %%%s\n",
                             x86_size_suffix(size), offset,
                             x86_gpr_name(def_phys, size));
    }
}

static void x86_emit_spill_store(x86_mir_emit_t *emit,
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

    int src_phys = x86_phys_of(emit, src_vreg);
    if (emit->failed) return;

    int offset = emit->spill_offsets[info->spill_slot];
    int size = x86_size_bytes(slot.size_bits);
    if (slot.reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, -%d(%%ebp)\n",
                             x86_fp_mov_op(size),
                             x86_xmm_names[src_phys], offset);
    } else {
        if (size < 4) size = 4;
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, -%d(%%ebp)\n",
                             x86_size_suffix(size),
                             x86_gpr_name(src_phys, size), offset);
    }
}

static void x86_emit_ret(x86_mir_emit_t *emit,
                         size_t instr_index,
                         const anvil_mir_instr_info_t *info)
{
    if (info->num_uses == 1) {
        anvil_mir_vreg_t ret = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        const anvil_mir_vreg_info_t *ret_info = x86_vreg_info_checked(emit, ret);
        int ret_phys = x86_phys_of(emit, ret);
        if (!ret_info || emit->failed) return;

        if (ret_info->reg_class == ANVIL_MIR_REG_FPR) {
            int size = x86_size_bytes(ret_info->size_bits);
            const char *mov = x86_fp_mov_op(size);
            const char *fld = size <= 4 ? "flds" : "fldl";
            anvil_strbuf_appendf(&emit->code, "\t%s %%%s, -%d(%%ebp)\n",
                                 mov, x86_xmm_names[ret_phys],
                                 emit->scratch_b_off);
            anvil_strbuf_appendf(&emit->code, "\t%s -%d(%%ebp)\n",
                                 fld, emit->scratch_b_off);
        } else {
            int size = x86_size_bytes(ret_info->size_bits);
            if (size < 4) size = 4;
            if (ret_phys != emit->desc->int_ret_reg) {
                anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n",
                                     x86_gpr_name(ret_phys, 4),
                                     x86_reg32_names[emit->desc->int_ret_reg]);
            }
        }
    }
    if (!emit->failed) x86_emit_epilogue(emit);
}

static void x86_emit_other(x86_mir_emit_t *emit,
                           const anvil_mir_instr_info_t *info)
{
    if (info->def == ANVIL_MIR_NO_VREG) {
        return;
    }
    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    if (!def_info || emit->failed) return;

    int phys = x86_phys_of(emit, info->def);
    if (emit->failed) return;

    if (def_info->reg_class == ANVIL_MIR_REG_GPR) {
        if (phys != emit->desc->int_ret_hi_reg) {
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n",
                                 x86_reg32_names[emit->desc->int_ret_hi_reg],
                                 x86_gpr_name(phys, 4));
        }
        return;
    }

    int size = x86_size_bytes(def_info->size_bits);
    const char *st = size <= 4 ? "fstps" : "fstpl";
    const char *mov = x86_fp_mov_op(size);
    anvil_strbuf_appendf(&emit->code, "\t%s -%d(%%ebp)\n", st, emit->scratch_b_off);
    anvil_strbuf_appendf(&emit->code, "\t%s -%d(%%ebp), %%%s\n",
                         mov, emit->scratch_b_off, x86_xmm_names[phys]);
}

static void x86_emit_instr(x86_mir_emit_t *emit,
                           size_t instr_index,
                           const anvil_mir_instr_info_t *info)
{
    switch (info->op) {
        case ANVIL_MIR_OP_MOV:
            x86_emit_mov(emit, info);
            break;
        case ANVIL_MIR_OP_COPY: {
            anvil_mir_vreg_t src =
                anvil_mir_get_instr_use(emit->mir, instr_index, 0);
            if (src == ANVIL_MIR_NO_VREG) {
                emit->failed = true;
                break;
            }
            x86_emit_copy(emit, info->def, src);
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
            x86_emit_binary(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_NEG:
        case ANVIL_MIR_OP_NOT:
        case ANVIL_MIR_OP_FABS:
            x86_emit_unary(emit, instr_index, info);
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
            x86_emit_cast(emit, instr_index, info);
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
            x86_emit_cmp(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_SYMBOL_ADDR:
            x86_emit_symbol_addr(emit, info);
            break;
        case ANVIL_MIR_OP_LOAD:
            x86_emit_load(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_STORE:
            x86_emit_store(emit, instr_index);
            break;
        case ANVIL_MIR_OP_FRAME_ADDR:
            x86_emit_frame_addr(emit, info);
            break;
        case ANVIL_MIR_OP_DYN_ALLOCA:
            x86_emit_dyn_alloca(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_INCOMING_STACK_ARG:
            x86_emit_incoming_stack_arg(emit, info);
            break;
        case ANVIL_MIR_OP_CALL:
            x86_emit_call(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_CALL_STACK_ARG:
            x86_emit_call_stack_arg(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_BR:
            anvil_strbuf_append(&emit->code, "\tjmp ");
            if (!x86_emit_branch_target(emit, info->true_block)) {
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
                x86_vreg_info_checked(emit, cond);
            if (!cond_info) break;
            const char *cond_reg = x86_gpr_name(x86_phys_of(emit, cond), 4);
            if (emit->failed) break;
            anvil_strbuf_appendf(&emit->code, "\ttestl %%%s, %%%s\n",
                                 cond_reg, cond_reg);
            anvil_strbuf_append(&emit->code, "\tjne ");
            if (!x86_emit_branch_target(emit, info->true_block)) {
                emit->failed = true;
                break;
            }
            anvil_strbuf_append(&emit->code, "\n\tjmp ");
            if (!x86_emit_branch_target(emit, info->false_block)) {
                emit->failed = true;
                break;
            }
            anvil_strbuf_append(&emit->code, "\n");
            break;
        }
        case ANVIL_MIR_OP_RET:
            x86_emit_ret(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_SPILL_LOAD:
            x86_emit_spill_load(emit, info);
            break;
        case ANVIL_MIR_OP_SPILL_STORE:
            x86_emit_spill_store(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_OTHER:
            if (info->def == ANVIL_MIR_NO_VREG && info->num_uses == 1) {
                anvil_mir_vreg_t hi =
                    anvil_mir_get_instr_use(emit->mir, instr_index, 0);
                int hi_phys = x86_phys_of(emit, hi);
                if (emit->failed) break;
                if (hi_phys != emit->desc->int_ret_hi_reg) {
                    anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n",
                                         x86_gpr_name(hi_phys, 4),
                                         x86_reg32_names[emit->desc->int_ret_hi_reg]);
                }
            } else {
                x86_emit_other(emit, info);
            }
            break;
        default:
            emit->failed = true;
            break;
    }
}

static void x86_emit_escaped_string(anvil_strbuf_t *code, const char *value)
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

static void x86_emit_rodata(x86_mir_emit_t *emit)
{
    size_t count = anvil_mir_num_string_literals(emit->mir);
    if (count == 0 && !emit->emitted_fneg_mask && !emit->emitted_fabs_mask) {
        return;
    }

    if (emit->plat->is_macho) {
        anvil_strbuf_append(&emit->code,
                            "\t.section __TEXT,__cstring,cstring_literals\n");
    } else if (emit->plat->is_coff) {
        anvil_strbuf_append(&emit->code, "\t.section .rdata,\"dr\"\n");
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
        x86_emit_escaped_string(&emit->code, info.value);
    }

    if (emit->emitted_fneg_mask) {
        anvil_strbuf_append(&emit->code, "\t.p2align 4\n");
        anvil_strbuf_append(&emit->code, ".Lx86_fneg_mask32:\n");
        anvil_strbuf_append(&emit->code, "\t.long 0x80000000\n");
        anvil_strbuf_append(&emit->code, "\t.long 0\n\t.long 0\n\t.long 0\n");
        anvil_strbuf_append(&emit->code, ".Lx86_fneg_mask64:\n");
        anvil_strbuf_append(&emit->code, "\t.long 0\n");
        anvil_strbuf_append(&emit->code, "\t.long 0x80000000\n");
        anvil_strbuf_append(&emit->code, "\t.long 0\n\t.long 0\n");
    }
    if (emit->emitted_fabs_mask) {
        anvil_strbuf_append(&emit->code, "\t.p2align 4\n");
        anvil_strbuf_append(&emit->code, ".Lx86_fabs_mask32:\n");
        anvil_strbuf_append(&emit->code, "\t.long 0x7fffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, ".Lx86_fabs_mask64:\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0x7fffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
    }
}

static int x86_compute_ret_pop(const anvil_mir_func_t *mir,
                               const anvil_x86_cc_desc_t *desc,
                               anvil_func_t *func)
{
    if (!desc->callee_cleans_stack || !func) return 0;
    (void)mir;
    int pop = 0;
    size_t int_reg_count = 0;
    for (size_t i = 0; i < func->num_params; i++) {
        anvil_value_t *param = func->params[i];
        anvil_type_t *type = param ? param->type : NULL;
        if (x86_needs_pair(type)) {
            pop += 8;
            continue;
        }
        if (!x86_type_is_fp(type) &&
            (int)int_reg_count < desc->num_reg_int_args) {
            int_reg_count++;
            continue;
        }
        pop += (int)x86_stack_arg_slot_size(type);
    }
    return pop;
}

bool anvil_x86_emit_mir_abi(const anvil_mir_func_t *mir,
                            anvil_func_t *func,
                            anvil_abi_t abi,
                            anvil_syntax_t syntax,
                            char **output,
                            size_t *len)
{
    if (!mir || !output) return false;
    if (!anvil_x86_verify_mir_legal(mir, NULL, 0)) return false;

    const anvil_x86_cc_desc_t *desc =
        anvil_x86_get_cc_desc(func ? func->cc : ANVIL_CC_CDECL);
    const anvil_x86_plat_desc_t *plat = anvil_x86_get_plat_desc(abi);
    if (!desc || !plat) return false;

    x86_mir_emit_t emit;
    memset(&emit, 0, sizeof(emit));
    emit.mir = mir;
    emit.desc = desc;
    emit.plat = plat;
    emit.syntax = syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS : syntax;
    emit.ret_pop_bytes = x86_compute_ret_pop(mir, desc, func);
    anvil_strbuf_init(&emit.code);
    if (!emit.code.data) return false;

    if (!x86_prepare_frame(&emit)) {
        anvil_strbuf_destroy(&emit.code);
        free(emit.spill_offsets);
        free(emit.frame_slot_offsets);
        return false;
    }

    x86_emit_func_header(&emit);
    x86_emit_prologue(&emit);

    size_t num_blocks = anvil_mir_num_blocks(mir);
    size_t num_instrs = anvil_mir_num_instrs(mir);
    for (size_t b = 0; b < num_blocks && !emit.failed; b++) {
        if (!x86_emit_label(&emit, (anvil_mir_block_t)b)) {
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
            x86_emit_instr(&emit, i, &info);
        }
    }

    if (!emit.failed && !emit.plat->is_macho && !emit.plat->is_coff) {
        const char *prefix = x86_symbol_prefix(&emit);
        const char *name = anvil_mir_func_name(mir);
        anvil_strbuf_appendf(&emit.code, "\t.size %s%s, .-%s%s\n",
                             prefix, name, prefix, name);
    }
    if (!emit.failed) {
        x86_emit_rodata(&emit);
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

bool anvil_x86_emit_mir(const anvil_mir_func_t *mir,
                        char **output,
                        size_t *len)
{
    return anvil_x86_emit_mir_abi(mir, NULL, ANVIL_ABI_SYSV,
                                  ANVIL_SYNTAX_GAS, output, len);
}
