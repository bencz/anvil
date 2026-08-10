/*
 * ANVIL - Shared IBM mainframe MachineIR backend.
 */

#include "anvil/anvil_mainframe_mir.h"
#include "anvil/anvil_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MF_INTERNAL_LABEL_CAP 160

typedef struct {
    anvil_value_t *value;
    anvil_mir_vreg_t vreg;
} mf_value_map_entry_t;

typedef struct {
    anvil_block_t *block;
    anvil_mir_block_t mir_block;
} mf_block_map_entry_t;

typedef struct {
    anvil_value_t *value;
    int frame_slot;
} mf_addr_map_entry_t;

typedef struct {
    anvil_value_t *value;
    int64_t imm;
} mf_wide_const_map_entry_t;

typedef struct {
    anvil_value_t *value;
    anvil_mir_vreg_t hi;
    anvil_mir_vreg_t lo;
    bool is_unsigned;
} mf_wide_pair_map_entry_t;

typedef struct {
    const anvil_mainframe_target_desc_t *desc;
    anvil_func_t *func;
    anvil_mir_func_t *mir;

    mf_value_map_entry_t *values;
    size_t num_values;
    size_t cap_values;

    mf_block_map_entry_t *blocks;
    size_t num_blocks;

    mf_addr_map_entry_t *frame_addrs;
    size_t num_frame_addrs;
    size_t cap_frame_addrs;

    mf_wide_const_map_entry_t *wide_consts;
    size_t num_wide_consts;
    size_t cap_wide_consts;

    mf_wide_pair_map_entry_t *wide_pairs;
    size_t num_wide_pairs;
    size_t cap_wide_pairs;
} mf_lower_t;

typedef struct {
    const anvil_mainframe_target_desc_t *desc;
    const anvil_mir_func_t *mir;
    anvil_fp_format_t fp_format;
    anvil_strbuf_t code;
    char func_label[96];

    int *frame_offsets;
    int *spill_offsets;
    int frame_size;
    int outgoing_values_offset;
    int outgoing_param_list_offset;
    int *call_arg_value_offsets;
    bool *call_arg_is_last;
    size_t *call_arg_counts;
    size_t max_call_args;
    unsigned label_counter;
} mf_emit_t;

static const int mf_alloc_gprs[] = { 2, 3, 4, 5, 6, 7, 8, 9, 10 };
static const int mf_scratch_gprs[] = { 0, 1 };
static const int mf_s370_fprs[] = { 2, 4, 6 };
static const int mf_s370_scratch_fprs[] = { 0 };
static const int mf_s390_fprs[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};
static const int mf_s390_scratch_fprs[] = { 0 };

static const anvil_mainframe_target_desc_t mf_target_descs[] = {
    {
        .variant = ANVIL_MAINFRAME_VARIANT_S370,
        .arch = ANVIL_ARCH_S370,
        .name = "IBM S/370",
        .ptr_size = 4,
        .addr_bits = 24,
        .word_size = 4,
        .num_fpr = 4,
        .save_area_size = 72,
        .fp_temp_offset = 72,
        .local_area_offset = 88,
        .amode = "24",
        .rmode = "24",
        .fp_format = ANVIL_FP_HFP,
        .big_endian = true,
        .has_64bit_gprs = false,
        .supports_ieee_fp = false,
        .supports_dfp = false,
        .has_relative_branches = false,
        .param_list_reg = 1,
        .return_addr_reg = 14,
        .return_gpr = 15,
        .return_fpr = 0,
        .base_reg = 12,
        .save_area_reg = 13,
        .alloc_gpr_regs = mf_alloc_gprs,
        .num_alloc_gpr_regs = sizeof(mf_alloc_gprs) / sizeof(mf_alloc_gprs[0]),
        .alloc_fpr_regs = mf_s370_fprs,
        .num_alloc_fpr_regs = sizeof(mf_s370_fprs) / sizeof(mf_s370_fprs[0]),
        .scratch_gpr_regs = mf_scratch_gprs,
        .num_scratch_gpr_regs = sizeof(mf_scratch_gprs) / sizeof(mf_scratch_gprs[0]),
        .scratch_fpr_regs = mf_s370_scratch_fprs,
        .num_scratch_fpr_regs = sizeof(mf_s370_scratch_fprs) / sizeof(mf_s370_scratch_fprs[0])
    },
    {
        .variant = ANVIL_MAINFRAME_VARIANT_S370_XA,
        .arch = ANVIL_ARCH_S370_XA,
        .name = "IBM S/370-XA",
        .ptr_size = 4,
        .addr_bits = 31,
        .word_size = 4,
        .num_fpr = 4,
        .save_area_size = 72,
        .fp_temp_offset = 72,
        .local_area_offset = 88,
        .amode = "31",
        .rmode = "ANY",
        .fp_format = ANVIL_FP_HFP,
        .big_endian = true,
        .has_64bit_gprs = false,
        .supports_ieee_fp = false,
        .supports_dfp = false,
        .has_relative_branches = false,
        .param_list_reg = 1,
        .return_addr_reg = 14,
        .return_gpr = 15,
        .return_fpr = 0,
        .base_reg = 12,
        .save_area_reg = 13,
        .alloc_gpr_regs = mf_alloc_gprs,
        .num_alloc_gpr_regs = sizeof(mf_alloc_gprs) / sizeof(mf_alloc_gprs[0]),
        .alloc_fpr_regs = mf_s370_fprs,
        .num_alloc_fpr_regs = sizeof(mf_s370_fprs) / sizeof(mf_s370_fprs[0]),
        .scratch_gpr_regs = mf_scratch_gprs,
        .num_scratch_gpr_regs = sizeof(mf_scratch_gprs) / sizeof(mf_scratch_gprs[0]),
        .scratch_fpr_regs = mf_s370_scratch_fprs,
        .num_scratch_fpr_regs = sizeof(mf_s370_scratch_fprs) / sizeof(mf_s370_scratch_fprs[0])
    },
    {
        .variant = ANVIL_MAINFRAME_VARIANT_S390,
        .arch = ANVIL_ARCH_S390,
        .name = "IBM S/390",
        .ptr_size = 4,
        .addr_bits = 31,
        .word_size = 4,
        .num_fpr = 16,
        .save_area_size = 72,
        .fp_temp_offset = 72,
        .local_area_offset = 88,
        .amode = "31",
        .rmode = "ANY",
        .fp_format = ANVIL_FP_HFP,
        .big_endian = true,
        .has_64bit_gprs = false,
        .supports_ieee_fp = true,
        .supports_dfp = false,
        .has_relative_branches = true,
        .param_list_reg = 1,
        .return_addr_reg = 14,
        .return_gpr = 15,
        .return_fpr = 0,
        .base_reg = 12,
        .save_area_reg = 13,
        .alloc_gpr_regs = mf_alloc_gprs,
        .num_alloc_gpr_regs = sizeof(mf_alloc_gprs) / sizeof(mf_alloc_gprs[0]),
        .alloc_fpr_regs = mf_s390_fprs,
        .num_alloc_fpr_regs = sizeof(mf_s390_fprs) / sizeof(mf_s390_fprs[0]),
        .scratch_gpr_regs = mf_scratch_gprs,
        .num_scratch_gpr_regs = sizeof(mf_scratch_gprs) / sizeof(mf_scratch_gprs[0]),
        .scratch_fpr_regs = mf_s390_scratch_fprs,
        .num_scratch_fpr_regs = sizeof(mf_s390_scratch_fprs) / sizeof(mf_s390_scratch_fprs[0])
    },
    {
        .variant = ANVIL_MAINFRAME_VARIANT_ZARCH,
        .arch = ANVIL_ARCH_ZARCH,
        .name = "IBM Z/ARCHITECTURE",
        .ptr_size = 8,
        .addr_bits = 64,
        .word_size = 8,
        .num_fpr = 16,
        .save_area_size = 144,
        .fp_temp_offset = 144,
        .local_area_offset = 160,
        .amode = "64",
        .rmode = "ANY",
        .fp_format = ANVIL_FP_HFP_IEEE,
        .big_endian = true,
        .has_64bit_gprs = true,
        .supports_ieee_fp = true,
        .supports_dfp = true,
        .has_relative_branches = true,
        .param_list_reg = 1,
        .return_addr_reg = 14,
        .return_gpr = 15,
        .return_fpr = 0,
        .base_reg = 12,
        .save_area_reg = 13,
        .alloc_gpr_regs = mf_alloc_gprs,
        .num_alloc_gpr_regs = sizeof(mf_alloc_gprs) / sizeof(mf_alloc_gprs[0]),
        .alloc_fpr_regs = mf_s390_fprs,
        .num_alloc_fpr_regs = sizeof(mf_s390_fprs) / sizeof(mf_s390_fprs[0]),
        .scratch_gpr_regs = mf_scratch_gprs,
        .num_scratch_gpr_regs = sizeof(mf_scratch_gprs) / sizeof(mf_scratch_gprs[0]),
        .scratch_fpr_regs = mf_s390_scratch_fprs,
        .num_scratch_fpr_regs = sizeof(mf_s390_scratch_fprs) / sizeof(mf_s390_scratch_fprs[0])
    }
};

const anvil_mainframe_target_desc_t *
anvil_mainframe_get_target_desc(anvil_mainframe_variant_t variant)
{
    if ((size_t)variant >= sizeof(mf_target_descs) / sizeof(mf_target_descs[0])) {
        return NULL;
    }
    return &mf_target_descs[variant];
}

static bool mf_type_is_fp(anvil_type_t *type)
{
    return type && (type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64);
}

static bool mf_type_is_void(anvil_type_t *type)
{
    return type && type->kind == ANVIL_TYPE_VOID;
}

static bool mf_type_is_signed(anvil_type_t *type)
{
    if (!type) return false;
    return type->kind == ANVIL_TYPE_I1 || type->kind == ANVIL_TYPE_I8 ||
           type->kind == ANVIL_TYPE_I16 ||
           type->kind == ANVIL_TYPE_I32 || type->kind == ANVIL_TYPE_I64;
}

static bool mf_type_is_64bit_integer(anvil_type_t *type)
{
    return type && (type->kind == ANVIL_TYPE_I64 ||
                    type->kind == ANVIL_TYPE_U64);
}

static anvil_mir_reg_class_t mf_reg_class_for_type(anvil_type_t *type)
{
    return mf_type_is_fp(type) ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR;
}

static uint16_t mf_bits_for_type(const anvil_mainframe_target_desc_t *desc,
                                 anvil_type_t *type)
{
    if (!type) return (uint16_t)(desc->word_size * 8);
    if (type->kind == ANVIL_TYPE_PTR || type->kind == ANVIL_TYPE_FUNC) {
        return (uint16_t)(desc->ptr_size * 8);
    }
    if (type->kind == ANVIL_TYPE_VOID) return 0;
    if (type->size == 0) return (uint16_t)(desc->word_size * 8);
    return (uint16_t)(type->size * 8);
}

static uint16_t mf_slot_bits_for_type(const anvil_mainframe_target_desc_t *desc,
                                      anvil_type_t *type)
{
    uint16_t bits = mf_bits_for_type(desc, type);
    uint16_t word_bits = (uint16_t)(desc->word_size * 8);
    return bits < word_bits ? word_bits : bits;
}

static uint16_t mf_align_for_type(const anvil_mainframe_target_desc_t *desc,
                                  anvil_type_t *type)
{
    if (!type) return (uint16_t)desc->word_size;
    if (type->align == 0) return 1;
    if (type->align > desc->word_size) return (uint16_t)desc->word_size;
    return (uint16_t)type->align;
}

static anvil_mir_vreg_t mf_add_vreg_for_type(mf_lower_t *lower,
                                             anvil_type_t *type)
{
    return anvil_mir_add_vreg_typed(
        lower->mir,
        mf_reg_class_for_type(type),
        mf_bits_for_type(lower->desc, type),
        mf_type_is_signed(type));
}

static bool mf_map_reserve(mf_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_values) return true;
    size_t cap = lower->cap_values ? lower->cap_values * 2 : 64;
    while (cap < needed) cap *= 2;
    mf_value_map_entry_t *grown =
        realloc(lower->values, cap * sizeof(*grown));
    if (!grown) return false;
    lower->values = grown;
    lower->cap_values = cap;
    return true;
}

static bool mf_map_put(mf_lower_t *lower, anvil_value_t *value,
                       anvil_mir_vreg_t vreg)
{
    if (!value) return true;
    for (size_t i = 0; i < lower->num_values; i++) {
        if (lower->values[i].value == value) {
            lower->values[i].vreg = vreg;
            return true;
        }
    }
    if (!mf_map_reserve(lower, lower->num_values + 1)) return false;
    lower->values[lower->num_values].value = value;
    lower->values[lower->num_values].vreg = vreg;
    lower->num_values++;
    return true;
}

static anvil_mir_vreg_t mf_map_get(mf_lower_t *lower, anvil_value_t *value)
{
    for (size_t i = 0; i < lower->num_values; i++) {
        if (lower->values[i].value == value) return lower->values[i].vreg;
    }
    return ANVIL_MIR_NO_VREG;
}

static bool mf_addr_map_reserve(mf_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_frame_addrs) return true;
    size_t cap = lower->cap_frame_addrs ? lower->cap_frame_addrs * 2 : 16;
    while (cap < needed) cap *= 2;
    mf_addr_map_entry_t *grown =
        realloc(lower->frame_addrs, cap * sizeof(*grown));
    if (!grown) return false;
    lower->frame_addrs = grown;
    lower->cap_frame_addrs = cap;
    return true;
}

static bool mf_addr_map_put(mf_lower_t *lower, anvil_value_t *value,
                            int frame_slot)
{
    if (!value) return false;
    for (size_t i = 0; i < lower->num_frame_addrs; i++) {
        if (lower->frame_addrs[i].value == value) {
            lower->frame_addrs[i].frame_slot = frame_slot;
            return true;
        }
    }
    if (!mf_addr_map_reserve(lower, lower->num_frame_addrs + 1)) return false;
    lower->frame_addrs[lower->num_frame_addrs].value = value;
    lower->frame_addrs[lower->num_frame_addrs].frame_slot = frame_slot;
    lower->num_frame_addrs++;
    return true;
}

static anvil_mir_block_t mf_block_get(mf_lower_t *lower, anvil_block_t *block)
{
    if (!block) return ANVIL_MIR_NO_BLOCK;
    for (size_t i = 0; i < lower->num_blocks; i++) {
        if (lower->blocks[i].block == block) return lower->blocks[i].mir_block;
    }
    return ANVIL_MIR_NO_BLOCK;
}

static bool mf_wide_const_reserve(mf_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_wide_consts) return true;
    size_t cap = lower->cap_wide_consts ? lower->cap_wide_consts * 2 : 8;
    while (cap < needed) cap *= 2;
    mf_wide_const_map_entry_t *grown =
        realloc(lower->wide_consts, cap * sizeof(*grown));
    if (!grown) return false;
    lower->wide_consts = grown;
    lower->cap_wide_consts = cap;
    return true;
}

static bool mf_wide_const_put(mf_lower_t *lower, anvil_value_t *value,
                              int64_t imm)
{
    if (!value) return false;
    for (size_t i = 0; i < lower->num_wide_consts; i++) {
        if (lower->wide_consts[i].value == value) {
            lower->wide_consts[i].imm = imm;
            return true;
        }
    }
    if (!mf_wide_const_reserve(lower, lower->num_wide_consts + 1)) return false;
    lower->wide_consts[lower->num_wide_consts].value = value;
    lower->wide_consts[lower->num_wide_consts].imm = imm;
    lower->num_wide_consts++;
    return true;
}

static bool mf_wide_const_get(mf_lower_t *lower, anvil_value_t *value,
                              int64_t *out)
{
    if (!value) return false;
    for (size_t i = 0; i < lower->num_wide_consts; i++) {
        if (lower->wide_consts[i].value == value) {
            if (out) *out = lower->wide_consts[i].imm;
            return true;
        }
    }
    return false;
}

static bool mf_wide_pair_reserve(mf_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_wide_pairs) return true;
    size_t cap = lower->cap_wide_pairs ? lower->cap_wide_pairs * 2 : 8;
    while (cap < needed) cap *= 2;
    mf_wide_pair_map_entry_t *grown =
        realloc(lower->wide_pairs, cap * sizeof(*grown));
    if (!grown) return false;
    lower->wide_pairs = grown;
    lower->cap_wide_pairs = cap;
    return true;
}

static bool mf_wide_pair_put(mf_lower_t *lower,
                             anvil_value_t *value,
                             anvil_mir_vreg_t hi,
                             anvil_mir_vreg_t lo,
                             bool is_unsigned)
{
    if (!value || hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG) return false;
    for (size_t i = 0; i < lower->num_wide_pairs; i++) {
        if (lower->wide_pairs[i].value == value) {
            lower->wide_pairs[i].hi = hi;
            lower->wide_pairs[i].lo = lo;
            lower->wide_pairs[i].is_unsigned = is_unsigned;
            return true;
        }
    }
    if (!mf_wide_pair_reserve(lower, lower->num_wide_pairs + 1)) return false;
    lower->wide_pairs[lower->num_wide_pairs].value = value;
    lower->wide_pairs[lower->num_wide_pairs].hi = hi;
    lower->wide_pairs[lower->num_wide_pairs].lo = lo;
    lower->wide_pairs[lower->num_wide_pairs].is_unsigned = is_unsigned;
    lower->num_wide_pairs++;
    return true;
}

static bool mf_wide_pair_get(mf_lower_t *lower,
                             anvil_value_t *value,
                             anvil_mir_vreg_t *hi,
                             anvil_mir_vreg_t *lo,
                             bool *is_unsigned)
{
    if (!value) return false;
    for (size_t i = 0; i < lower->num_wide_pairs; i++) {
        if (lower->wide_pairs[i].value == value) {
            if (hi) *hi = lower->wide_pairs[i].hi;
            if (lo) *lo = lower->wide_pairs[i].lo;
            if (is_unsigned) *is_unsigned = lower->wide_pairs[i].is_unsigned;
            return true;
        }
    }
    return false;
}

static void mf_uppercase(char *dest, const char *src, size_t max_len)
{
    size_t i = 0;
    if (!dest || max_len == 0) return;
    if (!src) src = "ANON";
    for (; i + 1 < max_len && src[i]; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            dest[i] = c;
        } else {
            dest[i] = '_';
        }
    }
    dest[i] = '\0';
}

static bool mf_create_mir_blocks(mf_lower_t *lower)
{
    lower->num_blocks = lower->func->num_blocks;
    lower->blocks = calloc(lower->num_blocks, sizeof(*lower->blocks));
    if (!lower->blocks && lower->num_blocks) return false;

    size_t idx = 0;
    for (anvil_block_t *block = lower->func->blocks; block; block = block->next) {
        anvil_mir_block_t mir_block = idx == 0
            ? anvil_mir_current_block(lower->mir)
            : anvil_mir_add_block(lower->mir, block->name);
        if (mir_block == ANVIL_MIR_NO_BLOCK) return false;
        lower->blocks[idx].block = block;
        lower->blocks[idx].mir_block = mir_block;
        idx++;
    }
    return true;
}

static bool mf_lower_params(mf_lower_t *lower)
{
    for (size_t i = 0; i < lower->func->num_params; i++) {
        anvil_value_t *param = lower->func->params[i];
        anvil_mir_vreg_t local = mf_add_vreg_for_type(lower, param->type);
        if (local == ANVIL_MIR_NO_VREG) return false;
        int64_t slot = (int64_t)i * (int64_t)lower->desc->ptr_size;
        if (!anvil_mir_add_instr_imm(lower->mir,
                                     ANVIL_MIR_OP_INCOMING_STACK_ARG,
                                     local, slot)) {
            return false;
        }
        if (!mf_map_put(lower, param, local)) return false;
    }
    return true;
}

static bool mf_prepare_phi_results(mf_lower_t *lower)
{
    for (anvil_block_t *block = lower->func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_PHI) continue;
            if (!instr->result) continue;
            anvil_mir_vreg_t vreg = mf_add_vreg_for_type(lower, instr->result->type);
            if (vreg == ANVIL_MIR_NO_VREG) return false;
            if (!mf_map_put(lower, instr->result, vreg)) return false;
        }
    }
    return true;
}

static anvil_mir_vreg_t mf_lower_value(mf_lower_t *lower, anvil_value_t *value);

static int64_t mf_int_constant(anvil_value_t *value)
{
    if (!value) return 0;
    if (value->type && !value->type->is_signed) return (int64_t)value->data.u;
    return value->data.i;
}

static bool mf_get_const_int(anvil_value_t *value, int64_t *out)
{
    if (!value || value->kind != ANVIL_VAL_CONST_INT) return false;
    if (out) *out = mf_int_constant(value);
    return true;
}

static anvil_mir_vreg_t mf_lower_const_value(mf_lower_t *lower,
                                             anvil_value_t *value)
{
    anvil_mir_vreg_t existing = mf_map_get(lower, value);
    if (existing != ANVIL_MIR_NO_VREG) return existing;

    anvil_mir_vreg_t vreg = mf_add_vreg_for_type(lower, value->type);
    if (vreg == ANVIL_MIR_NO_VREG) return ANVIL_MIR_NO_VREG;

    int64_t imm = 0;
    if (value->kind == ANVIL_VAL_CONST_NULL) {
        imm = 0;
    } else if (value->kind == ANVIL_VAL_CONST_FLOAT) {
        if (value->type && value->type->kind == ANVIL_TYPE_F32) {
            union { float f; uint32_t u; } cvt;
            cvt.f = (float)value->data.f;
            imm = (int64_t)cvt.u;
        } else {
            union { double d; uint64_t u; } cvt;
            cvt.d = value->data.f;
            imm = (int64_t)cvt.u;
        }
    } else {
        imm = mf_int_constant(value);
    }

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, vreg, imm)) {
        return ANVIL_MIR_NO_VREG;
    }
    if (!mf_map_put(lower, value, vreg)) return ANVIL_MIR_NO_VREG;
    return vreg;
}

static anvil_mir_vreg_t mf_lower_symbol_address(mf_lower_t *lower,
                                                anvil_type_t *type,
                                                const char *symbol)
{
    (void)type;
    anvil_mir_vreg_t vreg = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_GPR,
        (uint16_t)(lower->desc->ptr_size * 8), false);
    if (vreg == ANVIL_MIR_NO_VREG) return ANVIL_MIR_NO_VREG;

    if (!anvil_mir_add_instr_symbol(lower->mir, ANVIL_MIR_OP_SYMBOL_ADDR,
                                    vreg, NULL, 0, symbol)) {
        return ANVIL_MIR_NO_VREG;
    }
    return vreg;
}

static anvil_mir_vreg_t mf_lower_value(mf_lower_t *lower, anvil_value_t *value)
{
    if (!value) return ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t existing = mf_map_get(lower, value);
    if (existing != ANVIL_MIR_NO_VREG) return existing;

    switch (value->kind) {
        case ANVIL_VAL_CONST_INT:
        case ANVIL_VAL_CONST_FLOAT:
        case ANVIL_VAL_CONST_NULL:
            return mf_lower_const_value(lower, value);

        case ANVIL_VAL_CONST_STRING: {
            const char *label = NULL;
            if (anvil_mir_add_string_literal(lower->mir,
                                             value->data.str ? value->data.str : "",
                                             &label) < 0) {
                return ANVIL_MIR_NO_VREG;
            }
            anvil_mir_vreg_t vreg =
                mf_lower_symbol_address(lower, value->type, label);
            if (vreg == ANVIL_MIR_NO_VREG) return vreg;
            if (!mf_map_put(lower, value, vreg)) return ANVIL_MIR_NO_VREG;
            return vreg;
        }

        case ANVIL_VAL_GLOBAL:
        case ANVIL_VAL_FUNC: {
            anvil_mir_vreg_t vreg =
                mf_lower_symbol_address(lower, value->type, value->name);
            if (vreg == ANVIL_MIR_NO_VREG) return vreg;
            if (!mf_map_put(lower, value, vreg)) return ANVIL_MIR_NO_VREG;
            return vreg;
        }

        case ANVIL_VAL_PARAM:
        case ANVIL_VAL_INSTR:
            return existing;

        case ANVIL_VAL_CONST_DECIMAL:
        case ANVIL_VAL_CONST_ARRAY:
        case ANVIL_VAL_BLOCK:
            return ANVIL_MIR_NO_VREG;
    }

    return ANVIL_MIR_NO_VREG;
}

static bool mf_binary_op(anvil_op_t op, anvil_mir_opcode_t *out_op)
{
    switch (op) {
        case ANVIL_OP_ADD: *out_op = ANVIL_MIR_OP_ADD; return true;
        case ANVIL_OP_SUB: *out_op = ANVIL_MIR_OP_SUB; return true;
        case ANVIL_OP_MUL: *out_op = ANVIL_MIR_OP_MUL; return true;
        case ANVIL_OP_SDIV: *out_op = ANVIL_MIR_OP_SDIV; return true;
        case ANVIL_OP_UDIV: *out_op = ANVIL_MIR_OP_UDIV; return true;
        case ANVIL_OP_SMOD: *out_op = ANVIL_MIR_OP_SMOD; return true;
        case ANVIL_OP_UMOD: *out_op = ANVIL_MIR_OP_UMOD; return true;
        case ANVIL_OP_AND: *out_op = ANVIL_MIR_OP_AND; return true;
        case ANVIL_OP_OR: *out_op = ANVIL_MIR_OP_OR; return true;
        case ANVIL_OP_XOR: *out_op = ANVIL_MIR_OP_XOR; return true;
        case ANVIL_OP_SHL: *out_op = ANVIL_MIR_OP_SHL; return true;
        case ANVIL_OP_SHR: *out_op = ANVIL_MIR_OP_SHR; return true;
        case ANVIL_OP_SAR: *out_op = ANVIL_MIR_OP_SAR; return true;
        case ANVIL_OP_CMP_EQ: *out_op = ANVIL_MIR_OP_CMP_EQ; return true;
        case ANVIL_OP_CMP_NE: *out_op = ANVIL_MIR_OP_CMP_NE; return true;
        case ANVIL_OP_CMP_LT: *out_op = ANVIL_MIR_OP_CMP_LT; return true;
        case ANVIL_OP_CMP_LE: *out_op = ANVIL_MIR_OP_CMP_LE; return true;
        case ANVIL_OP_CMP_GT: *out_op = ANVIL_MIR_OP_CMP_GT; return true;
        case ANVIL_OP_CMP_GE: *out_op = ANVIL_MIR_OP_CMP_GE; return true;
        case ANVIL_OP_CMP_ULT: *out_op = ANVIL_MIR_OP_CMP_ULT; return true;
        case ANVIL_OP_CMP_ULE: *out_op = ANVIL_MIR_OP_CMP_ULE; return true;
        case ANVIL_OP_CMP_UGT: *out_op = ANVIL_MIR_OP_CMP_UGT; return true;
        case ANVIL_OP_CMP_UGE: *out_op = ANVIL_MIR_OP_CMP_UGE; return true;
        case ANVIL_OP_FADD: *out_op = ANVIL_MIR_OP_ADD; return true;
        case ANVIL_OP_FSUB: *out_op = ANVIL_MIR_OP_SUB; return true;
        case ANVIL_OP_FMUL: *out_op = ANVIL_MIR_OP_MUL; return true;
        case ANVIL_OP_FDIV: *out_op = ANVIL_MIR_OP_FDIV; return true;
        case ANVIL_OP_FCMP: *out_op = ANVIL_MIR_OP_FCMP; return true;
        default: return false;
    }
}

static bool mf_unary_op(anvil_op_t op, anvil_mir_opcode_t *out_op)
{
    switch (op) {
        case ANVIL_OP_NEG:
        case ANVIL_OP_FNEG: *out_op = ANVIL_MIR_OP_NEG; return true;
        case ANVIL_OP_NOT: *out_op = ANVIL_MIR_OP_NOT; return true;
        case ANVIL_OP_FABS: *out_op = ANVIL_MIR_OP_FABS; return true;
        case ANVIL_OP_ZEXT: *out_op = ANVIL_MIR_OP_ZEXT; return true;
        case ANVIL_OP_SEXT: *out_op = ANVIL_MIR_OP_SEXT; return true;
        case ANVIL_OP_TRUNC: *out_op = ANVIL_MIR_OP_TRUNC; return true;
        case ANVIL_OP_BITCAST:
        case ANVIL_OP_PTRTOINT:
        case ANVIL_OP_INTTOPTR: *out_op = ANVIL_MIR_OP_BITCAST; return true;
        case ANVIL_OP_SITOFP: *out_op = ANVIL_MIR_OP_SITOFP; return true;
        case ANVIL_OP_UITOFP: *out_op = ANVIL_MIR_OP_UITOFP; return true;
        case ANVIL_OP_FPTOSI: *out_op = ANVIL_MIR_OP_FPTOSI; return true;
        case ANVIL_OP_FPTOUI: *out_op = ANVIL_MIR_OP_FPTOUI; return true;
        case ANVIL_OP_FPEXT: *out_op = ANVIL_MIR_OP_FPEXT; return true;
        case ANVIL_OP_FPTRUNC: *out_op = ANVIL_MIR_OP_FPTRUNC; return true;
        default: return false;
    }
}

static bool mf_add_return_copy(mf_lower_t *lower, anvil_value_t *value)
{
    if (!value) {
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET,
                                   ANVIL_MIR_NO_VREG, NULL, 0);
    }

    anvil_mir_vreg_t src = mf_lower_value(lower, value);
    if (src == ANVIL_MIR_NO_VREG) return false;

    anvil_mir_vreg_t ret = mf_add_vreg_for_type(lower, value->type);
    if (ret == ANVIL_MIR_NO_VREG) return false;
    if (!anvil_mir_set_fixed_reg(lower->mir, ret,
                                 mf_type_is_fp(value->type)
                                     ? lower->desc->return_fpr
                                     : lower->desc->return_gpr)) {
        return false;
    }

    anvil_mir_vreg_t uses[] = { src };
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, ret, uses, 1)) {
        return false;
    }
    uses[0] = ret;
    return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET,
                               ANVIL_MIR_NO_VREG, uses, 1);
}

static bool mf_emit_phi_copies_for_edge(mf_lower_t *lower,
                                        anvil_block_t *pred,
                                        anvil_block_t *succ)
{
    if (!succ) return true;
    typedef struct {
        anvil_mir_vreg_t dst;
        anvil_mir_vreg_t src;
    } mf_phi_copy_t;
    mf_phi_copy_t *copies = NULL;
    size_t count = 0;
    size_t capacity = 0;
    for (anvil_instr_t *phi = succ->first; phi; phi = phi->next) {
        if (phi->op != ANVIL_OP_PHI) break;
        anvil_value_t *incoming = NULL;
        for (size_t i = 0; i < phi->num_phi_incoming; i++) {
            if (phi->phi_blocks && phi->phi_blocks[i] == pred) {
                incoming = phi->operands[i];
                break;
            }
        }
        if (!incoming || !phi->result) continue;
        anvil_mir_vreg_t src = mf_lower_value(lower, incoming);
        anvil_mir_vreg_t dst = mf_map_get(lower, phi->result);
        if (src == ANVIL_MIR_NO_VREG || dst == ANVIL_MIR_NO_VREG) {
            free(copies);
            return false;
        }
        if (src == dst) continue;
        if (count == capacity) {
            size_t grown_capacity = capacity ? capacity * 2 : 4;
            mf_phi_copy_t *grown = realloc(
                copies, grown_capacity * sizeof(*grown));
            if (!grown) {
                free(copies);
                return false;
            }
            copies = grown;
            capacity = grown_capacity;
        }
        copies[count++] = (mf_phi_copy_t){ dst, src };
    }

    while (count > 0) {
        size_t ready = count;
        for (size_t i = 0; i < count; i++) {
            bool destination_is_still_needed = false;
            for (size_t j = 0; j < count; j++) {
                if (i != j && copies[j].src == copies[i].dst) {
                    destination_is_still_needed = true;
                    break;
                }
            }
            if (!destination_is_still_needed) {
                ready = i;
                break;
            }
        }

        if (ready == count) {
            const anvil_mir_vreg_info_t *info =
                anvil_mir_get_vreg_info(lower->mir, copies[0].src);
            if (!info) {
                free(copies);
                return false;
            }
            anvil_mir_vreg_t temp = anvil_mir_add_vreg_typed(
                lower->mir, info->reg_class, info->size_bits,
                info->is_signed);
            anvil_mir_vreg_t temp_uses[] = { copies[0].src };
            if (temp == ANVIL_MIR_NO_VREG ||
                !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                                     temp, temp_uses, 1)) {
                free(copies);
                return false;
            }
            copies[0].src = temp;
            continue;
        }

        anvil_mir_vreg_t uses[] = { copies[ready].src };
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                                 copies[ready].dst, uses, 1)) {
            free(copies);
            return false;
        }
        copies[ready] = copies[count - 1];
        count--;
    }
    free(copies);
    return true;
}

static bool mf_block_has_phi(anvil_block_t *block)
{
    return block && block->first && block->first->op == ANVIL_OP_PHI;
}

static anvil_mir_block_t mf_create_phi_edge_block(mf_lower_t *lower,
                                                  anvil_block_t *pred,
                                                  anvil_block_t *succ)
{
    if (!mf_block_has_phi(succ)) return mf_block_get(lower, succ);

    char name[160];
    snprintf(name, sizeof(name), "%s_to_%s_phi",
             pred && pred->name ? pred->name : "pred",
             succ && succ->name ? succ->name : "succ");
    anvil_mir_block_t previous = anvil_mir_current_block(lower->mir);
    anvil_mir_block_t edge = anvil_mir_add_block(lower->mir, name);
    if (edge == ANVIL_MIR_NO_BLOCK) return edge;

    if (!anvil_mir_set_current_block(lower->mir, edge)) return ANVIL_MIR_NO_BLOCK;
    if (!mf_emit_phi_copies_for_edge(lower, pred, succ)) return ANVIL_MIR_NO_BLOCK;
    if (!anvil_mir_add_branch(lower->mir, mf_block_get(lower, succ))) {
        return ANVIL_MIR_NO_BLOCK;
    }
    if (!anvil_mir_set_current_block(lower->mir, previous)) {
        return ANVIL_MIR_NO_BLOCK;
    }
    return edge;
}

static bool mf_lower_alloca(mf_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || !instr->result->type ||
        instr->result->type->kind != ANVIL_TYPE_PTR) {
        return false;
    }

    anvil_type_t *elem = instr->aux_type ? instr->aux_type
                                         : instr->result->type->data.pointee;
    if (instr->num_operands == 0) {
        int slot = anvil_mir_add_frame_slot(lower->mir,
                                            mf_slot_bits_for_type(lower->desc, elem),
                                            mf_align_for_type(lower->desc, elem));
        if (slot < 0) return false;
        anvil_mir_vreg_t ptr = mf_add_vreg_for_type(lower, instr->result->type);
        if (ptr == ANVIL_MIR_NO_VREG) return false;
        if (!anvil_mir_add_frame_addr(lower->mir, ptr, slot)) return false;
        if (!mf_addr_map_put(lower, instr->result, slot)) return false;
        return mf_map_put(lower, instr->result, ptr);
    }

    anvil_mir_vreg_t count = mf_lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t ptr = mf_add_vreg_for_type(lower, instr->result->type);
    if (count == ANVIL_MIR_NO_VREG || ptr == ANVIL_MIR_NO_VREG) return false;
    int64_t elem_size = elem && elem->size ? (int64_t)elem->size : 1;
    anvil_mir_vreg_t uses[] = { count };
    if (!anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_DYN_ALLOCA,
                                      ptr, uses, 1, elem_size)) {
        return false;
    }
    return mf_map_put(lower, instr->result, ptr);
}

static bool mf_lower_add_const_offset(mf_lower_t *lower,
                                      anvil_mir_vreg_t base,
                                      int64_t offset,
                                      anvil_mir_vreg_t dst)
{
    if (offset == 0) {
        anvil_mir_vreg_t uses[] = { base };
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, dst, uses, 1);
    }

    anvil_mir_vreg_t off = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_GPR,
        (uint16_t)(lower->desc->ptr_size * 8), true);
    if (off == ANVIL_MIR_NO_VREG) return false;
    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, off, offset)) {
        return false;
    }
    anvil_mir_vreg_t uses[] = { base, off };
    return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_ADD, dst, uses, 2);
}

static anvil_mir_vreg_t mf_normalize_gep_index(mf_lower_t *lower,
                                                anvil_mir_vreg_t index,
                                                anvil_type_t *index_type)
{
    const anvil_mir_vreg_info_t *info =
        anvil_mir_get_vreg_info(lower->mir, index);
    uint16_t ptr_bits = (uint16_t)(lower->desc->ptr_size * 8);
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR) return ANVIL_MIR_NO_VREG;
    if (info->size_bits == ptr_bits) return index;
    bool is_signed = index_type && index_type->is_signed;
    anvil_mir_vreg_t normalized = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_GPR, ptr_bits, is_signed);
    if (normalized == ANVIL_MIR_NO_VREG) return normalized;
    anvil_mir_opcode_t op = info->size_bits > ptr_bits
                                ? ANVIL_MIR_OP_TRUNC
                                : (is_signed ? ANVIL_MIR_OP_SEXT
                                             : ANVIL_MIR_OP_ZEXT);
    anvil_mir_vreg_t uses[] = { index };
    if (!anvil_mir_add_instr(lower->mir, op, normalized, uses, 1))
        return ANVIL_MIR_NO_VREG;
    return normalized;
}

static bool mf_get_wide_const_value(mf_lower_t *lower, anvil_value_t *value,
                                    int64_t *out)
{
    if (mf_get_const_int(value, out)) return true;
    return mf_wide_const_get(lower, value, out);
}

static bool mf_lower_store_i64_const(mf_lower_t *lower,
                                     int64_t imm,
                                     anvil_mir_vreg_t addr)
{
    uint64_t bits = (uint64_t)imm;
    int32_t hi = (int32_t)(bits >> 32);
    int32_t lo = (int32_t)(bits & 0xffffffffu);

    anvil_mir_vreg_t hi_reg = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_GPR, 32, true);
    anvil_mir_vreg_t lo_reg = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_GPR, 32, true);
    anvil_mir_vreg_t lo_addr = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_GPR,
        (uint16_t)(lower->desc->ptr_size * 8), false);
    if (hi_reg == ANVIL_MIR_NO_VREG || lo_reg == ANVIL_MIR_NO_VREG ||
        lo_addr == ANVIL_MIR_NO_VREG) {
        return false;
    }

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV,
                                 hi_reg, (int64_t)hi)) {
        return false;
    }
    anvil_mir_vreg_t hi_uses[] = { hi_reg, addr };
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_STORE,
                             ANVIL_MIR_NO_VREG, hi_uses, 2)) {
        return false;
    }

    if (!mf_lower_add_const_offset(lower, addr, 4, lo_addr)) return false;
    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV,
                                 lo_reg, (int64_t)lo)) {
        return false;
    }
    anvil_mir_vreg_t lo_uses[] = { lo_reg, lo_addr };
    return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_STORE,
                               ANVIL_MIR_NO_VREG, lo_uses, 2);
}

static anvil_mir_vreg_t mf_add_i32_vreg(mf_lower_t *lower, bool is_signed)
{
    return anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32,
                                    is_signed);
}

static anvil_mir_vreg_t mf_add_bool_vreg(mf_lower_t *lower)
{
    return anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
}

static bool mf_lower_load_i64_pair(mf_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || instr->num_operands < 1) return false;
    anvil_mir_vreg_t addr = mf_lower_value(lower, instr->operands[0]);
    if (addr == ANVIL_MIR_NO_VREG) return false;

    bool is_unsigned = instr->result->type &&
                       instr->result->type->kind == ANVIL_TYPE_U64;
    anvil_mir_vreg_t hi = mf_add_i32_vreg(lower, !is_unsigned);
    anvil_mir_vreg_t lo = mf_add_i32_vreg(lower, false);
    anvil_mir_vreg_t lo_addr = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_GPR,
        (uint16_t)(lower->desc->ptr_size * 8), false);
    if (hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG ||
        lo_addr == ANVIL_MIR_NO_VREG) {
        return false;
    }

    anvil_mir_vreg_t hi_uses[] = { addr };
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_LOAD,
                             hi, hi_uses, 1)) {
        return false;
    }
    if (!mf_lower_add_const_offset(lower, addr, 4, lo_addr)) return false;
    anvil_mir_vreg_t lo_uses[] = { lo_addr };
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_LOAD,
                             lo, lo_uses, 1)) {
        return false;
    }
    return mf_wide_pair_put(lower, instr->result, hi, lo, is_unsigned);
}

static bool mf_lower_store_i64_pair(mf_lower_t *lower,
                                    anvil_value_t *value,
                                    anvil_mir_vreg_t addr)
{
    anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
    if (!mf_wide_pair_get(lower, value, &hi, &lo, NULL)) return false;

    anvil_mir_vreg_t hi_uses[] = { hi, addr };
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_STORE,
                             ANVIL_MIR_NO_VREG, hi_uses, 2)) {
        return false;
    }

    anvil_mir_vreg_t lo_addr = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_GPR,
        (uint16_t)(lower->desc->ptr_size * 8), false);
    if (lo_addr == ANVIL_MIR_NO_VREG) return false;
    if (!mf_lower_add_const_offset(lower, addr, 4, lo_addr)) return false;
    anvil_mir_vreg_t lo_uses[] = { lo, lo_addr };
    return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_STORE,
                               ANVIL_MIR_NO_VREG, lo_uses, 2);
}

static bool mf_lower_i64_bitcast_pair(mf_lower_t *lower, anvil_instr_t *instr)
{
    anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
    if (!instr->result || instr->num_operands < 1 ||
        !mf_wide_pair_get(lower, instr->operands[0], &hi, &lo, NULL)) {
        return false;
    }
    bool is_unsigned = instr->result->type &&
                       instr->result->type->kind == ANVIL_TYPE_U64;
    return mf_wide_pair_put(lower, instr->result, hi, lo, is_unsigned);
}

static bool mf_add_pair_cmp(mf_lower_t *lower,
                            anvil_mir_opcode_t op,
                            anvil_mir_vreg_t def,
                            anvil_mir_vreg_t lhs,
                            anvil_mir_vreg_t rhs)
{
    anvil_mir_vreg_t uses[] = { lhs, rhs };
    return anvil_mir_add_instr(lower->mir, op, def, uses, 2);
}

static bool mf_lower_i64_cmp_pair(mf_lower_t *lower,
                                  anvil_instr_t *instr,
                                  anvil_mir_opcode_t op)
{
    if (!instr->result || instr->num_operands < 2) return false;
    anvil_mir_vreg_t lhi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t llo = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t rhi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t rlo = ANVIL_MIR_NO_VREG;
    if (!mf_wide_pair_get(lower, instr->operands[0], &lhi, &llo, NULL) ||
        !mf_wide_pair_get(lower, instr->operands[1], &rhi, &rlo, NULL)) {
        return false;
    }

    anvil_mir_vreg_t result = mf_add_bool_vreg(lower);
    if (result == ANVIL_MIR_NO_VREG) return false;

    if (op == ANVIL_MIR_OP_CMP_EQ || op == ANVIL_MIR_OP_CMP_NE) {
        anvil_mir_vreg_t hi_cmp = mf_add_bool_vreg(lower);
        anvil_mir_vreg_t lo_cmp = mf_add_bool_vreg(lower);
        if (hi_cmp == ANVIL_MIR_NO_VREG || lo_cmp == ANVIL_MIR_NO_VREG) {
            return false;
        }
        if (!mf_add_pair_cmp(lower, op, hi_cmp, lhi, rhi) ||
            !mf_add_pair_cmp(lower, op, lo_cmp, llo, rlo)) {
            return false;
        }
        anvil_mir_vreg_t uses[] = { hi_cmp, lo_cmp };
        anvil_mir_opcode_t join = op == ANVIL_MIR_OP_CMP_EQ
                                      ? ANVIL_MIR_OP_AND
                                      : ANVIL_MIR_OP_OR;
        if (!anvil_mir_add_instr(lower->mir, join, result, uses, 2)) return false;
        return mf_map_put(lower, instr->result, result);
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

    anvil_mir_vreg_t hi_cmp = mf_add_bool_vreg(lower);
    anvil_mir_vreg_t hi_eq = mf_add_bool_vreg(lower);
    anvil_mir_vreg_t lo_cmp = mf_add_bool_vreg(lower);
    anvil_mir_vreg_t eq_and_lo = mf_add_bool_vreg(lower);
    if (hi_cmp == ANVIL_MIR_NO_VREG || hi_eq == ANVIL_MIR_NO_VREG ||
        lo_cmp == ANVIL_MIR_NO_VREG || eq_and_lo == ANVIL_MIR_NO_VREG) {
        return false;
    }
    if (!mf_add_pair_cmp(lower, hi_rel, hi_cmp, lhi, rhi) ||
        !mf_add_pair_cmp(lower, ANVIL_MIR_OP_CMP_EQ, hi_eq, lhi, rhi) ||
        !mf_add_pair_cmp(lower, lo_rel, lo_cmp, llo, rlo)) {
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
    return mf_map_put(lower, instr->result, result);
}

static bool mf_lower_gep(mf_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || instr->num_operands < 2 || !instr->aux_type)
        return false;
    anvil_mir_vreg_t cur = mf_lower_value(lower, instr->operands[0]);
    if (cur == ANVIL_MIR_NO_VREG) return false;

    anvil_type_t *walk_type = instr->aux_type;
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
        if (step.kind != ANVIL_GEP_STEP_SCALE) return false;
        int64_t elem_size = (int64_t)step.amount;
        anvil_mir_vreg_t idx = mf_lower_value(lower, index_value);
        if (idx == ANVIL_MIR_NO_VREG) return false;
        idx = mf_normalize_gep_index(lower, idx, index_value->type);
        if (idx == ANVIL_MIR_NO_VREG) return false;

        anvil_mir_vreg_t scaled = idx;
        if (elem_size != 1) {
            anvil_mir_vreg_t scale = anvil_mir_add_vreg_typed(
                lower->mir, ANVIL_MIR_REG_GPR,
                (uint16_t)(lower->desc->ptr_size * 8), true);
            scaled = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR,
                                              (uint16_t)(lower->desc->ptr_size * 8),
                                              true);
            if (scale == ANVIL_MIR_NO_VREG || scaled == ANVIL_MIR_NO_VREG) {
                return false;
            }
            if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV,
                                         scale, elem_size)) {
                return false;
            }
            anvil_mir_vreg_t mul_uses[] = { idx, scale };
            if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_MUL,
                                     scaled, mul_uses, 2)) {
                return false;
            }
        }

        anvil_mir_vreg_t next = mf_add_vreg_for_type(lower, instr->result->type);
        if (next == ANVIL_MIR_NO_VREG) return false;
        anvil_mir_vreg_t add_uses[] = { cur, scaled };
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_ADD,
                                 next, add_uses, 2)) {
            return false;
        }
        cur = next;
    }

    if (constant_offset != 0) {
        anvil_mir_vreg_t next = mf_add_vreg_for_type(lower, instr->result->type);
        if (next == ANVIL_MIR_NO_VREG ||
            !mf_lower_add_const_offset(lower, cur, constant_offset, next)) {
            return false;
        }
        cur = next;
    }

    return mf_map_put(lower, instr->result, cur);
}

static bool mf_lower_struct_gep(mf_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || instr->num_operands < 2 || !instr->aux_type ||
        instr->aux_type->kind != ANVIL_TYPE_STRUCT ||
        instr->operands[1]->kind != ANVIL_VAL_CONST_INT) {
        return false;
    }
    uint64_t field = instr->operands[1]->type && !instr->operands[1]->type->is_signed
                         ? instr->operands[1]->data.u
                         : (uint64_t)instr->operands[1]->data.i;
    if (field >= instr->aux_type->data.struc.num_fields) return false;

    anvil_mir_vreg_t base = mf_lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t dst = mf_add_vreg_for_type(lower, instr->result->type);
    if (base == ANVIL_MIR_NO_VREG || dst == ANVIL_MIR_NO_VREG) return false;

    if (!mf_lower_add_const_offset(lower, base,
                                   (int64_t)instr->aux_type->data.struc.offsets[field],
                                   dst)) {
        return false;
    }
    return mf_map_put(lower, instr->result, dst);
}

static bool mf_lower_call(mf_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands == 0) return false;

    size_t num_args = instr->num_operands - 1;
    anvil_mir_vreg_t *arg_vregs = num_args
        ? calloc(num_args, sizeof(*arg_vregs)) : NULL;
    if (num_args && !arg_vregs) return false;
    /* Materializing constants/symbols can itself append MIR. Resolve every
       value before starting the contiguous CALL_STACK_ARG bundle. */
    for (size_t i = 0; i < num_args; i++) {
        arg_vregs[i] = mf_lower_value(lower, instr->operands[i + 1]);
        if (arg_vregs[i] == ANVIL_MIR_NO_VREG) {
            free(arg_vregs);
            return false;
        }
    }
    for (size_t i = 0; i < num_args; i++) {
        anvil_mir_vreg_t uses[] = { arg_vregs[i] };
        if (!anvil_mir_add_instr_imm_uses(lower->mir,
                                          ANVIL_MIR_OP_CALL_STACK_ARG,
                                          ANVIL_MIR_NO_VREG,
                                          uses, 1, (int64_t)i)) {
            free(arg_vregs);
            return false;
        }
    }
    free(arg_vregs);

    anvil_mir_vreg_t call_def = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t result_vreg = ANVIL_MIR_NO_VREG;
    if (instr->result && !mf_type_is_void(instr->result->type)) {
        call_def = mf_add_vreg_for_type(lower, instr->result->type);
        if (call_def == ANVIL_MIR_NO_VREG) return false;
        if (!anvil_mir_set_fixed_reg(lower->mir, call_def,
                                     mf_type_is_fp(instr->result->type)
                                         ? lower->desc->return_fpr
                                         : lower->desc->return_gpr)) {
            return false;
        }
    }

    bool ok = false;
    anvil_value_t *callee = instr->operands[0];
    if (callee->kind == ANVIL_VAL_FUNC) {
        ok = anvil_mir_add_instr_symbol(lower->mir, ANVIL_MIR_OP_CALL,
                                        call_def, NULL, 0, callee->name);
    } else {
        anvil_mir_vreg_t callee_vreg = mf_lower_value(lower, callee);
        if (callee_vreg == ANVIL_MIR_NO_VREG) return false;
        anvil_mir_vreg_t uses[] = { callee_vreg };
        ok = anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_CALL,
                                 call_def, uses, 1);
    }
    if (!ok) return false;

    if (instr->result) {
        if (mf_type_is_void(instr->result->type)) {
            return true;
        }
        result_vreg = mf_add_vreg_for_type(lower, instr->result->type);
        if (result_vreg == ANVIL_MIR_NO_VREG) return false;
        anvil_mir_vreg_t uses[] = { call_def };
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                                 result_vreg, uses, 1)) {
            return false;
        }
        return mf_map_put(lower, instr->result, result_vreg);
    }
    return true;
}

static bool mf_lower_switch(mf_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands == 0 || !instr->true_block) return false;
    anvil_mir_vreg_t selector = mf_lower_value(lower, instr->operands[0]);
    if (selector == ANVIL_MIR_NO_VREG) return false;

    anvil_mir_block_t default_block =
        mf_create_phi_edge_block(lower, instr->parent, instr->true_block);
    if (default_block == ANVIL_MIR_NO_BLOCK) return false;

    for (size_t i = 0; i < instr->num_switch_cases; i++) {
        if (!instr->switch_blocks || i + 1 >= instr->num_operands) return false;
        anvil_mir_block_t case_target =
            mf_create_phi_edge_block(lower, instr->parent, instr->switch_blocks[i]);
        if (case_target == ANVIL_MIR_NO_BLOCK) return false;

        char block_name[96];
        snprintf(block_name, sizeof(block_name), "switch_case_%zu", i);
        anvil_mir_block_t next_block = anvil_mir_add_block(lower->mir, block_name);
        if (next_block == ANVIL_MIR_NO_BLOCK) return false;

        anvil_mir_vreg_t case_val = mf_lower_value(lower, instr->operands[i + 1]);
        anvil_mir_vreg_t cond = anvil_mir_add_vreg_typed(
            lower->mir, ANVIL_MIR_REG_GPR, 8, false);
        if (case_val == ANVIL_MIR_NO_VREG || cond == ANVIL_MIR_NO_VREG) {
            return false;
        }
        anvil_mir_vreg_t cmp_uses[] = { selector, case_val };
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_CMP_EQ,
                                 cond, cmp_uses, 2)) {
            return false;
        }
        if (!anvil_mir_add_cond_branch(lower->mir, cond,
                                       case_target, next_block)) {
            return false;
        }
        if (!anvil_mir_set_current_block(lower->mir, next_block)) return false;
    }

    return anvil_mir_add_branch(lower->mir, default_block);
}

static bool mf_lower_instr(mf_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr) return true;
    if (instr->op == ANVIL_OP_PHI || instr->op == ANVIL_OP_NOP) return true;

    anvil_mir_opcode_t op;
    if (mf_binary_op(instr->op, &op)) {
        if (!instr->result || instr->num_operands < 2) return false;
        if (!lower->desc->has_64bit_gprs &&
            mf_type_is_64bit_integer(instr->operands[0]->type) &&
            mf_type_is_64bit_integer(instr->operands[1]->type) &&
            (op == ANVIL_MIR_OP_CMP_EQ ||
             op == ANVIL_MIR_OP_CMP_NE ||
             op == ANVIL_MIR_OP_CMP_LT ||
             op == ANVIL_MIR_OP_CMP_LE ||
             op == ANVIL_MIR_OP_CMP_GT ||
             op == ANVIL_MIR_OP_CMP_GE ||
             op == ANVIL_MIR_OP_CMP_ULT ||
             op == ANVIL_MIR_OP_CMP_ULE ||
             op == ANVIL_MIR_OP_CMP_UGT ||
             op == ANVIL_MIR_OP_CMP_UGE)) {
            return mf_lower_i64_cmp_pair(lower, instr, op);
        }
        anvil_mir_vreg_t lhs = mf_lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t rhs = mf_lower_value(lower, instr->operands[1]);
        anvil_mir_vreg_t def = mf_add_vreg_for_type(lower, instr->result->type);
        if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG ||
            def == ANVIL_MIR_NO_VREG) {
            return false;
        }
        anvil_mir_vreg_t uses[] = { lhs, rhs };
        bool added = op == ANVIL_MIR_OP_FCMP
            ? anvil_mir_add_instr_imm_uses(lower->mir, op, def, uses, 2,
                                           instr->fcmp_pred)
            : anvil_mir_add_instr(lower->mir, op, def, uses, 2);
        if (!added) return false;
        return mf_map_put(lower, instr->result, def);
    }

    if (mf_unary_op(instr->op, &op)) {
        if (!instr->result || instr->num_operands < 1) return false;
        if (!lower->desc->has_64bit_gprs &&
            mf_type_is_64bit_integer(instr->result->type)) {
            if (instr->op == ANVIL_OP_BITCAST) {
                return mf_lower_i64_bitcast_pair(lower, instr);
            }
            int64_t imm = 0;
            if (!mf_get_wide_const_value(lower, instr->operands[0], &imm)) {
                return false;
            }
            if (instr->op == ANVIL_OP_NEG) {
                return mf_wide_const_put(lower, instr->result,
                                         (int64_t)(0 - (uint64_t)imm));
            }
            if (instr->op == ANVIL_OP_NOT) {
                return mf_wide_const_put(lower, instr->result, ~imm);
            }
            return false;
        }
        anvil_mir_vreg_t src = mf_lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t def = mf_add_vreg_for_type(lower, instr->result->type);
        if (src == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) return false;
        if (instr->result->type->kind == ANVIL_TYPE_I1 &&
            instr->operands[0]->type->kind != ANVIL_TYPE_I1) {
            const anvil_mir_vreg_info_t *src_info =
                anvil_mir_get_vreg_info(lower->mir, src);
            if (src_info && src_info->reg_class == ANVIL_MIR_REG_FPR) {
                if (op != ANVIL_MIR_OP_FPTOUI) return false;
                anvil_mir_vreg_t converted = anvil_mir_add_vreg_typed(
                    lower->mir, ANVIL_MIR_REG_GPR, 32, false);
                anvil_mir_vreg_t convert_use[] = { src };
                if (converted == ANVIL_MIR_NO_VREG ||
                    !anvil_mir_add_instr(lower->mir, op, converted,
                                         convert_use, 1)) return false;
                src = converted;
                src_info = anvil_mir_get_vreg_info(lower->mir, src);
            }
            if (!src_info || src_info->reg_class != ANVIL_MIR_REG_GPR)
                return false;
            anvil_mir_vreg_t narrowed = src;
            if (src_info->size_bits > 8) {
                narrowed = anvil_mir_add_vreg_typed(
                    lower->mir, ANVIL_MIR_REG_GPR, 8, false);
                anvil_mir_vreg_t narrow_use[] = { src };
                if (narrowed == ANVIL_MIR_NO_VREG ||
                    !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_TRUNC,
                                         narrowed, narrow_use, 1)) return false;
            }
            anvil_mir_vreg_t one = anvil_mir_add_vreg_typed(
                lower->mir, ANVIL_MIR_REG_GPR, 8, false);
            anvil_mir_vreg_t norm_uses[] = { narrowed, one };
            if (one == ANVIL_MIR_NO_VREG ||
                !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, one, 1) ||
                !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_AND, def,
                                     norm_uses, 2)) return false;
            return mf_map_put(lower, instr->result, def);
        }
        anvil_mir_vreg_t uses[] = { src };
        if (!anvil_mir_add_instr(lower->mir, op, def, uses, 1)) return false;
        return mf_map_put(lower, instr->result, def);
    }

    switch (instr->op) {
        case ANVIL_OP_ALLOCA:
            return mf_lower_alloca(lower, instr);

        case ANVIL_OP_GEP:
            return mf_lower_gep(lower, instr);

        case ANVIL_OP_STRUCT_GEP:
            return mf_lower_struct_gep(lower, instr);

        case ANVIL_OP_LOAD: {
            if (!instr->result || instr->num_operands < 1) return false;
            if (!lower->desc->has_64bit_gprs &&
                mf_type_is_64bit_integer(instr->result->type)) {
                return mf_lower_load_i64_pair(lower, instr);
            }
            anvil_mir_vreg_t addr = mf_lower_value(lower, instr->operands[0]);
            anvil_mir_vreg_t def = mf_add_vreg_for_type(lower, instr->result->type);
            if (addr == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) return false;
            bool is_i1 = instr->result->type->kind == ANVIL_TYPE_I1;
            anvil_mir_vreg_t loaded = is_i1
                ? anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false)
                : def;
            if (loaded == ANVIL_MIR_NO_VREG) return false;
            anvil_mir_vreg_t uses[] = { addr };
            if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_LOAD, loaded, uses, 1)) {
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
            return mf_map_put(lower, instr->result, def);
        }

        case ANVIL_OP_STORE: {
            if (instr->num_operands < 2) return false;
            if (!lower->desc->has_64bit_gprs &&
                mf_type_is_64bit_integer(instr->operands[0]->type)) {
                int64_t imm = 0;
                if (mf_get_wide_const_value(lower, instr->operands[0], &imm)) {
                    anvil_mir_vreg_t addr = mf_lower_value(lower, instr->operands[1]);
                    if (addr == ANVIL_MIR_NO_VREG) return false;
                    return mf_lower_store_i64_const(lower, imm, addr);
                }
                if (mf_wide_pair_get(lower, instr->operands[0], NULL, NULL, NULL)) {
                    anvil_mir_vreg_t addr = mf_lower_value(lower, instr->operands[1]);
                    if (addr == ANVIL_MIR_NO_VREG) return false;
                    return mf_lower_store_i64_pair(lower, instr->operands[0], addr);
                }
            }
            anvil_mir_vreg_t value = mf_lower_value(lower, instr->operands[0]);
            anvil_mir_vreg_t addr = mf_lower_value(lower, instr->operands[1]);
            if (value == ANVIL_MIR_NO_VREG || addr == ANVIL_MIR_NO_VREG) return false;
            if (instr->operands[0]->type->kind == ANVIL_TYPE_I1) {
                anvil_mir_vreg_t one = anvil_mir_add_vreg_typed(
                    lower->mir, ANVIL_MIR_REG_GPR, 8, false);
                anvil_mir_vreg_t normalized = anvil_mir_add_vreg_typed(
                    lower->mir, ANVIL_MIR_REG_GPR, 8, false);
                anvil_mir_vreg_t norm_uses[] = { value, one };
                if (one == ANVIL_MIR_NO_VREG || normalized == ANVIL_MIR_NO_VREG ||
                    !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, one, 1) ||
                    !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_AND, normalized,
                                         norm_uses, 2)) return false;
                value = normalized;
            }
            anvil_mir_vreg_t uses[] = { value, addr };
            return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_STORE,
                                       ANVIL_MIR_NO_VREG, uses, 2);
        }

        case ANVIL_OP_CALL:
            return mf_lower_call(lower, instr);

        case ANVIL_OP_RET:
            return mf_add_return_copy(lower,
                                      instr->num_operands ? instr->operands[0] : NULL);

        case ANVIL_OP_BR: {
            anvil_mir_block_t target =
                mf_create_phi_edge_block(lower, instr->parent, instr->true_block);
            return target != ANVIL_MIR_NO_BLOCK &&
                   anvil_mir_add_branch(lower->mir, target);
        }

        case ANVIL_OP_BR_COND: {
            if (instr->num_operands < 1) return false;
            anvil_mir_vreg_t cond = mf_lower_value(lower, instr->operands[0]);
            anvil_mir_block_t true_block =
                mf_create_phi_edge_block(lower, instr->parent, instr->true_block);
            anvil_mir_block_t false_block =
                mf_create_phi_edge_block(lower, instr->parent, instr->false_block);
            return cond != ANVIL_MIR_NO_VREG &&
                   true_block != ANVIL_MIR_NO_BLOCK &&
                   false_block != ANVIL_MIR_NO_BLOCK &&
                   anvil_mir_add_cond_branch(lower->mir, cond,
                                             true_block, false_block);
        }

        case ANVIL_OP_SWITCH:
            return mf_lower_switch(lower, instr);

        case ANVIL_OP_SELECT: {
            if (!instr->result || instr->num_operands < 3) return false;
            anvil_mir_vreg_t cond = mf_lower_value(lower, instr->operands[0]);
            anvil_mir_vreg_t tv = mf_lower_value(lower, instr->operands[1]);
            anvil_mir_vreg_t fv = mf_lower_value(lower, instr->operands[2]);
            anvil_mir_vreg_t def = mf_add_vreg_for_type(lower, instr->result->type);
            if (cond == ANVIL_MIR_NO_VREG || tv == ANVIL_MIR_NO_VREG ||
                fv == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) {
                return false;
            }
            anvil_mir_vreg_t uses[] = { cond, tv, fv };
            if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_SELECT,
                                     def, uses, 3)) {
                return false;
            }
            return mf_map_put(lower, instr->result, def);
        }

        default:
            return false;
    }
}

anvil_mir_func_t *anvil_mainframe_lower_func_to_mir(
    anvil_func_t *func,
    anvil_mainframe_variant_t variant)
{
    const anvil_mainframe_target_desc_t *desc =
        anvil_mainframe_get_target_desc(variant);
    if (!func || !desc || !func->entry) return NULL;

    anvil_mir_func_t *mir = anvil_mir_func_create(func->name);
    if (!mir) return NULL;

    mf_lower_t lower;
    memset(&lower, 0, sizeof(lower));
    lower.desc = desc;
    lower.func = func;
    lower.mir = mir;

    bool ok = mf_create_mir_blocks(&lower);
    if (ok) ok = anvil_mir_set_current_block(mir, mf_block_get(&lower, func->entry));
    if (ok) ok = mf_lower_params(&lower);
    if (ok) ok = mf_prepare_phi_results(&lower);

    for (anvil_block_t *block = func->blocks; ok && block; block = block->next) {
        ok = anvil_mir_set_current_block(mir, mf_block_get(&lower, block));
        for (anvil_instr_t *instr = block->first; ok && instr; instr = instr->next) {
            ok = mf_lower_instr(&lower, instr);
        }
    }

    free(lower.values);
    free(lower.blocks);
    free(lower.frame_addrs);
    free(lower.wide_consts);
    free(lower.wide_pairs);

    char verify_error[256] = { 0 };
    bool verified = ok && anvil_mir_verify(mir, verify_error,
                                            sizeof(verify_error));
    if (!verified) {
        if (func->parent && func->parent->ctx) {
            anvil_set_error(func->parent->ctx, ANVIL_ERR_CODEGEN,
                            "Mainframe MIR lowering failed: %s",
                            ok ? verify_error : "unsupported IR operation");
        }
        anvil_mir_func_destroy(mir);
        return NULL;
    }
    return mir;
}

static bool mf_legal_fail(char *error, size_t error_len, const char *fmt, ...)
{
    if (error && error_len > 0) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(error, error_len, fmt, args);
        va_end(args);
    }
    return false;
}

static bool mf_size_legal(const anvil_mainframe_target_desc_t *desc,
                          const anvil_mir_vreg_info_t *info)
{
    if (!info) return false;
    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        return info->size_bits == 32 || info->size_bits == 64;
    }
    if (info->reg_class != ANVIL_MIR_REG_GPR) return false;
    if (info->size_bits == 8 || info->size_bits == 16 ||
        info->size_bits == 32) {
        return true;
    }
    return desc->has_64bit_gprs && info->size_bits == 64;
}

static const anvil_mir_vreg_info_t *mf_vreg_info(
    const anvil_mainframe_target_desc_t *desc,
    const anvil_mir_func_t *mir,
    anvil_mir_vreg_t vreg,
    char *error,
    size_t error_len)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, vreg);
    if (!info) {
        mf_legal_fail(error, error_len, "invalid vreg %u", vreg);
        return NULL;
    }
    if (!mf_size_legal(desc, info)) {
        mf_legal_fail(error, error_len,
                      "illegal vreg %u size %u for %s",
                      vreg, info->size_bits, desc->name);
        return NULL;
    }
    return info;
}

static bool mf_verify_call_bundles(const anvil_mir_func_t *mir,
                                   char *error, size_t error_len)
{
    size_t num_instrs = anvil_mir_num_instrs(mir);
    for (size_t i = 0; i < num_instrs; i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, i, &instr)) {
            return mf_legal_fail(error, error_len,
                                 "cannot inspect instruction %zu", i);
        }
        if (instr.op != ANVIL_MIR_OP_CALL_STACK_ARG) continue;

        size_t expected = 0;
        anvil_mir_block_t block = instr.block;
        do {
            if (!instr.has_imm || instr.imm != (int64_t)expected ||
                instr.num_uses != 1 || instr.block != block) {
                return mf_legal_fail(error, error_len,
                    "malformed call argument bundle at instruction %zu", i);
            }
            expected++;
            i++;
            if (i >= num_instrs ||
                !anvil_mir_get_instr_info(mir, i, &instr)) {
                return mf_legal_fail(error, error_len,
                    "unterminated call argument bundle");
            }
        } while (instr.op == ANVIL_MIR_OP_CALL_STACK_ARG &&
                 instr.block == block);

        if (instr.op != ANVIL_MIR_OP_CALL || instr.block != block) {
            return mf_legal_fail(error, error_len,
                "call argument bundle is not immediately followed by its call");
        }
    }
    return true;
}

static bool mf_verify_numeric_cast(const anvil_mir_func_t *mir, size_t index,
                                   anvil_mir_instr_info_t instr,
                                   char *error, size_t error_len)
{
    if (instr.num_uses != 1 || instr.def == ANVIL_MIR_NO_VREG) {
        return mf_legal_fail(error, error_len,
                             "numeric conversion must have one source and one result");
    }
    const anvil_mir_vreg_info_t *dst =
        anvil_mir_get_vreg_info(mir, instr.def);
    anvil_mir_vreg_t src_vreg = anvil_mir_get_instr_use(mir, index, 0);
    const anvil_mir_vreg_info_t *src =
        anvil_mir_get_vreg_info(mir, src_vreg);
    if (!src || !dst) return mf_legal_fail(error, error_len, "invalid conversion vreg");

    bool valid = false;
    switch (instr.op) {
        case ANVIL_MIR_OP_SITOFP:
        case ANVIL_MIR_OP_UITOFP:
            valid = src->reg_class == ANVIL_MIR_REG_GPR &&
                    dst->reg_class == ANVIL_MIR_REG_FPR &&
                    (src->size_bits == 8 || src->size_bits == 16 ||
                     src->size_bits == 32 || src->size_bits == 64) &&
                    (dst->size_bits == 32 || dst->size_bits == 64);
            break;
        case ANVIL_MIR_OP_FPTOSI:
        case ANVIL_MIR_OP_FPTOUI:
            valid = src->reg_class == ANVIL_MIR_REG_FPR &&
                    dst->reg_class == ANVIL_MIR_REG_GPR &&
                    (src->size_bits == 32 || src->size_bits == 64) &&
                    (dst->size_bits == 8 || dst->size_bits == 16 ||
                     dst->size_bits == 32 || dst->size_bits == 64);
            break;
        case ANVIL_MIR_OP_FPEXT:
            valid = src->reg_class == ANVIL_MIR_REG_FPR &&
                    dst->reg_class == ANVIL_MIR_REG_FPR &&
                    src->size_bits == 32 && dst->size_bits == 64;
            break;
        case ANVIL_MIR_OP_FPTRUNC:
            valid = src->reg_class == ANVIL_MIR_REG_FPR &&
                    dst->reg_class == ANVIL_MIR_REG_FPR &&
                    src->size_bits == 64 && dst->size_bits == 32;
            break;
        default:
            return true;
    }
    return valid || mf_legal_fail(error, error_len,
                                  "invalid numeric conversion register classes or widths");
}

bool anvil_mainframe_verify_mir_legal(
    const anvil_mir_func_t *mir,
    anvil_mainframe_variant_t variant,
    char *error,
    size_t error_len)
{
    const anvil_mainframe_target_desc_t *desc =
        anvil_mainframe_get_target_desc(variant);
    if (!desc || !mir) {
        return mf_legal_fail(error, error_len, "invalid mainframe MIR input");
    }
    if (!anvil_mir_verify(mir, error, error_len)) return false;
    if (!mf_verify_call_bundles(mir, error, error_len)) return false;

    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, i, &instr)) {
            return mf_legal_fail(error, error_len, "cannot inspect instruction %zu", i);
        }
        if (instr.def != ANVIL_MIR_NO_VREG &&
            !mf_vreg_info(desc, mir, instr.def, error, error_len)) {
            return false;
        }
        for (size_t u = 0; u < instr.num_uses; u++) {
            anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, i, u);
            if (!mf_vreg_info(desc, mir, use, error, error_len)) return false;
        }

        switch (instr.op) {
            case ANVIL_MIR_OP_SITOFP:
            case ANVIL_MIR_OP_UITOFP:
            case ANVIL_MIR_OP_FPTOSI:
            case ANVIL_MIR_OP_FPTOUI:
            case ANVIL_MIR_OP_FPEXT:
            case ANVIL_MIR_OP_FPTRUNC:
                if (!mf_verify_numeric_cast(mir, i, instr, error, error_len)) {
                    return false;
                }
                if (!desc->has_64bit_gprs) {
                    const anvil_mir_vreg_info_t *dst =
                        anvil_mir_get_vreg_info(mir, instr.def);
                    anvil_mir_vreg_t src_vreg = anvil_mir_get_instr_use(mir, i, 0);
                    const anvil_mir_vreg_info_t *src =
                        anvil_mir_get_vreg_info(mir, src_vreg);
                    if ((src && src->reg_class == ANVIL_MIR_REG_GPR && src->size_bits > 32) ||
                        (dst && dst->reg_class == ANVIL_MIR_REG_GPR && dst->size_bits > 32)) {
                        return mf_legal_fail(error, error_len,
                            "%s cannot represent a 64-bit integer conversion vreg",
                            desc->name);
                    }
                }
                break;
            case ANVIL_MIR_OP_SMOD:
            case ANVIL_MIR_OP_UMOD:
                break;
            case ANVIL_MIR_OP_KEEPALIVE:
                break;
            case ANVIL_MIR_OP_CALL_RESULT:
            case ANVIL_MIR_OP_RET_VALUE_PART:
            case ANVIL_MIR_OP_INVALID:
                return mf_legal_fail(error, error_len,
                    "%s has illegal MachineIR opcode",
                    desc->name);
            default:
                break;
        }
    }
    return true;
}

bool anvil_mainframe_regalloc_mir(anvil_mir_func_t *mir,
                                  anvil_mainframe_variant_t variant)
{
    const anvil_mainframe_target_desc_t *desc =
        anvil_mainframe_get_target_desc(variant);
    if (!desc || !mir) return false;
    if (!anvil_mainframe_verify_mir_legal(mir, variant, NULL, 0)) return false;

    anvil_regalloc_class_config_t configs[] = {
        {
            .reg_class = ANVIL_MIR_REG_GPR,
            .num_phys_regs = (int)desc->num_alloc_gpr_regs,
            .phys_regs = desc->alloc_gpr_regs
        },
        {
            .reg_class = ANVIL_MIR_REG_FPR,
            .num_phys_regs = (int)desc->num_alloc_fpr_regs,
            .phys_regs = desc->alloc_fpr_regs
        }
    };
    anvil_regalloc_class_config_t scratch_configs[] = {
        {
            .reg_class = ANVIL_MIR_REG_GPR,
            .num_phys_regs = (int)desc->num_scratch_gpr_regs,
            .phys_regs = desc->scratch_gpr_regs
        },
        {
            .reg_class = ANVIL_MIR_REG_FPR,
            .num_phys_regs = (int)desc->num_scratch_fpr_regs,
            .phys_regs = desc->scratch_fpr_regs
        }
    };

    if (!anvil_regalloc_linear_scan_classes(mir, configs,
                                            sizeof(configs) / sizeof(configs[0]))) {
        return false;
    }
    if (!anvil_mir_materialize_spills(mir, scratch_configs,
                                      sizeof(scratch_configs) / sizeof(scratch_configs[0]))) {
        return false;
    }
    return anvil_mainframe_verify_mir_legal(mir, variant, NULL, 0);
}

static const char *mf_gpr_name(int reg)
{
    static const char *names[] = {
        "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
        "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"
    };
    if (reg < 0 || reg > 15) return "R?";
    return names[reg];
}

static const char *mf_fpr_name(int reg)
{
    static const char *names[] = {
        "F0", "F1", "F2", "F3", "F4", "F5", "F6", "F7",
        "F8", "F9", "F10", "F11", "F12", "F13", "F14", "F15"
    };
    if (reg < 0 || reg > 15) return "F?";
    return names[reg];
}

static const anvil_mir_vreg_info_t *mf_emit_vreg_info(mf_emit_t *emit,
                                                      anvil_mir_vreg_t vreg)
{
    return anvil_mir_get_vreg_info(emit->mir, vreg);
}

static const anvil_regalloc_assignment_t *mf_assignment(mf_emit_t *emit,
                                                       anvil_mir_vreg_t vreg)
{
    return anvil_mir_get_assignment(emit->mir, vreg);
}

static int mf_phys_reg(mf_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *a = mf_assignment(emit, vreg);
    return a ? a->phys_reg : 0;
}

static const char *mf_vreg_reg_name(mf_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = mf_emit_vreg_info(emit, vreg);
    int reg = mf_phys_reg(emit, vreg);
    return info && info->reg_class == ANVIL_MIR_REG_FPR
               ? mf_fpr_name(reg)
               : mf_gpr_name(reg);
}

static int mf_size_bytes(uint16_t bits)
{
    return bits <= 8 ? 1 : bits <= 16 ? 2 : bits <= 32 ? 4 : 8;
}

static int mf_storage_bytes(uint16_t bits)
{
    return (int)((bits + 7u) / 8u);
}

static int mf_align_int(int value, int align)
{
    if (align <= 1) return value;
    return (value + align - 1) & ~(align - 1);
}

static bool mf_prepare_frame(mf_emit_t *emit)
{
    size_t num_frame = anvil_mir_num_frame_slots(emit->mir);
    size_t num_spills = anvil_mir_num_spills(emit->mir);
    size_t num_instrs = anvil_mir_num_instrs(emit->mir);
    emit->frame_offsets = calloc(num_frame ? num_frame : 1, sizeof(int));
    emit->spill_offsets = calloc(num_spills ? num_spills : 1, sizeof(int));
    emit->call_arg_value_offsets = malloc((num_instrs ? num_instrs : 1) *
                                          sizeof(*emit->call_arg_value_offsets));
    emit->call_arg_is_last = calloc(num_instrs ? num_instrs : 1,
                                    sizeof(*emit->call_arg_is_last));
    emit->call_arg_counts = calloc(num_instrs ? num_instrs : 1,
                                   sizeof(*emit->call_arg_counts));
    if (!emit->frame_offsets || !emit->spill_offsets ||
        !emit->call_arg_value_offsets || !emit->call_arg_is_last ||
        !emit->call_arg_counts) {
        return false;
    }
    for (size_t i = 0; i < num_instrs; i++) {
        emit->call_arg_value_offsets[i] = -1;
    }

    int offset = (int)emit->desc->local_area_offset;
    for (size_t i = 0; i < num_frame; i++) {
        anvil_mir_frame_slot_info_t slot;
        if (!anvil_mir_get_frame_slot_info(emit->mir, (int)i, &slot)) return false;
        int align = slot.align_bytes ? slot.align_bytes : 1;
        offset = mf_align_int(offset, align);
        emit->frame_offsets[i] = offset;
        offset += mf_storage_bytes(slot.size_bits);
    }

    for (size_t i = 0; i < num_spills; i++) {
        anvil_mir_spill_slot_info_t spill;
        if (!anvil_mir_get_spill_slot_info(emit->mir, (int)i, &spill)) return false;
        int size = mf_size_bytes(spill.size_bits);
        offset = mf_align_int(offset, size > 8 ? 8 : size);
        emit->spill_offsets[i] = offset;
        offset += size;
    }

    emit->max_call_args = 0;
    int max_call_value_bytes = 0;
    for (size_t i = 0; i < num_instrs; i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(emit->mir, i, &instr)) return false;
        if (instr.op != ANVIL_MIR_OP_CALL_STACK_ARG) continue;

        size_t count = 0;
        int value_bytes = 0;
        anvil_mir_block_t block = instr.block;
        while (i < num_instrs) {
            if (!anvil_mir_get_instr_info(emit->mir, i, &instr)) return false;
            if (instr.op != ANVIL_MIR_OP_CALL_STACK_ARG || instr.block != block) break;
            if (!instr.has_imm || instr.imm != (int64_t)count ||
                instr.num_uses != 1) {
                return false;
            }
            anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, i, 0);
            const anvil_mir_vreg_info_t *info = mf_emit_vreg_info(emit, value);
            if (!info) return false;
            int size = mf_size_bytes(info->size_bits);
            int align = size > 8 ? 8 : size;
            value_bytes = mf_align_int(value_bytes, align);
            emit->call_arg_value_offsets[i] = value_bytes;
            value_bytes += size;
            count++;
            i++;
        }
        if (i >= num_instrs ||
            !anvil_mir_get_instr_info(emit->mir, i, &instr) ||
            instr.block != block || instr.op != ANVIL_MIR_OP_CALL) {
            return false;
        }
        emit->call_arg_is_last[i - 1] = true;
        emit->call_arg_counts[i] = count;
        if (count > emit->max_call_args) emit->max_call_args = count;
        if (value_bytes > max_call_value_bytes) {
            max_call_value_bytes = value_bytes;
        }
    }

    if (emit->max_call_args > 0) {
        offset = mf_align_int(offset, (int)emit->desc->word_size);
        emit->outgoing_values_offset = offset;
        offset += max_call_value_bytes;
        offset = mf_align_int(offset, (int)emit->desc->ptr_size);
        emit->outgoing_param_list_offset = offset;
        offset += (int)(emit->max_call_args * emit->desc->ptr_size);
    } else {
        emit->outgoing_values_offset = -1;
        emit->outgoing_param_list_offset = -1;
    }

    emit->frame_size = mf_align_int(offset, emit->desc->ptr_size == 8 ? 16 : 8);
    return true;
}

static void mf_emit_load_imm(mf_emit_t *emit, int reg, int64_t imm)
{
    if (emit->desc->has_64bit_gprs) {
        if (imm >= -32768 && imm <= 32767) {
            anvil_strbuf_appendf(&emit->code,
                "         LGHI  %-4s,%lld\n", mf_gpr_name(reg), (long long)imm);
        } else if (imm >= -2147483648LL && imm <= 2147483647LL) {
            anvil_strbuf_appendf(&emit->code,
                "         LGFI  %-4s,%lld\n", mf_gpr_name(reg), (long long)imm);
        } else {
            anvil_strbuf_appendf(&emit->code,
                "         LG    %-4s,=FD'%lld'\n", mf_gpr_name(reg), (long long)imm);
        }
        return;
    }

    if (imm >= 0 && imm <= 4095) {
        anvil_strbuf_appendf(&emit->code,
            "         LA    %-4s,%lld\n", mf_gpr_name(reg), (long long)imm);
    } else {
        anvil_strbuf_appendf(&emit->code,
            "         L     %-4s,=F'%lld'\n", mf_gpr_name(reg), (long long)imm);
    }
}

static bool mf_use_ieee_fp(mf_emit_t *emit)
{
    return emit->fp_format == ANVIL_FP_IEEE754 ||
           emit->fp_format == ANVIL_FP_HFP_IEEE;
}

static void mf_emit_load_fp_imm(mf_emit_t *emit, int reg,
                                uint16_t size_bits, int64_t imm)
{
    if (size_bits == 32) {
        union { uint32_t u; float f; } cvt;
        cvt.u = (uint32_t)imm;
        anvil_strbuf_appendf(&emit->code, "         LE    %-4s,=%s'%g'\n",
            mf_fpr_name(reg), mf_use_ieee_fp(emit) ? "EB" : "E", cvt.f);
    } else {
        union { uint64_t u; double d; } cvt;
        cvt.u = (uint64_t)imm;
        anvil_strbuf_appendf(&emit->code, "         LD    %-4s,=%s'%g'\n",
            mf_fpr_name(reg), mf_use_ieee_fp(emit) ? "DB" : "D", cvt.d);
    }
}

static void mf_emit_header(mf_emit_t *emit)
{
    anvil_strbuf_append(&emit->code, "***********************************************************************\n");
    anvil_strbuf_appendf(&emit->code, "*        GENERATED BY ANVIL FOR %s\n", emit->desc->name);
    anvil_strbuf_append(&emit->code, "***********************************************************************\n");
    anvil_strbuf_append(&emit->code, "         CSECT\n");
    anvil_strbuf_appendf(&emit->code, "         AMODE %s\n", emit->desc->amode);
    anvil_strbuf_appendf(&emit->code, "         RMODE %s\n", emit->desc->rmode);
}

static void mf_emit_prologue(mf_emit_t *emit)
{
    anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", emit->func_label);
    if (emit->desc->has_64bit_gprs) {
        anvil_strbuf_append(&emit->code, "         STMG  R14,R12,24(R13)\n");
        anvil_strbuf_append(&emit->code, "         LGR   R12,R15\n");
        anvil_strbuf_appendf(&emit->code, "         USING %s,R12\n", emit->func_label);
        anvil_strbuf_append(&emit->code, "         LGR   R11,R13\n");
        if (emit->frame_size <= 4095) {
            anvil_strbuf_appendf(&emit->code, "         LA    R2,%d(,R13)\n",
                                 emit->frame_size);
        } else {
            mf_emit_load_imm(emit, 2, emit->frame_size);
            anvil_strbuf_append(&emit->code, "         AGR   R2,R13\n");
        }
        anvil_strbuf_append(&emit->code, "         STG   R11,8(,R2)\n");
        anvil_strbuf_append(&emit->code, "         STG   R2,16(,R11)\n");
        anvil_strbuf_append(&emit->code, "         LGR   R13,R2\n");
    } else {
        anvil_strbuf_append(&emit->code, "         STM   R14,R12,12(R13)\n");
        anvil_strbuf_append(&emit->code, "         LR    R12,R15\n");
        anvil_strbuf_appendf(&emit->code, "         USING %s,R12\n", emit->func_label);
        anvil_strbuf_append(&emit->code, "         LR    R11,R13\n");
        if (emit->frame_size <= 4095) {
            anvil_strbuf_appendf(&emit->code, "         LA    R2,%d(,R13)\n",
                                 emit->frame_size);
        } else {
            mf_emit_load_imm(emit, 2, emit->frame_size);
            anvil_strbuf_append(&emit->code, "         AR    R2,R13\n");
        }
        anvil_strbuf_append(&emit->code, "         ST    R11,4(,R2)\n");
        anvil_strbuf_append(&emit->code, "         ST    R2,8(,R11)\n");
        anvil_strbuf_append(&emit->code, "         LR    R13,R2\n");
    }
}

static void mf_emit_epilogue(mf_emit_t *emit)
{
    if (emit->desc->has_64bit_gprs) {
        anvil_strbuf_append(&emit->code, "         LG    R13,8(,R13)\n");
        anvil_strbuf_append(&emit->code, "         LG    R14,24(,R13)\n");
        anvil_strbuf_append(&emit->code, "         LMG   R0,R12,40(R13)\n");
    } else {
        anvil_strbuf_append(&emit->code, "         L     R13,4(,R13)\n");
        anvil_strbuf_append(&emit->code, "         L     R14,12(,R13)\n");
        anvil_strbuf_append(&emit->code, "         LM    R0,R12,20(R13)\n");
    }
    anvil_strbuf_append(&emit->code, "         BR    R14\n");
}

static void mf_block_label(mf_emit_t *emit, anvil_mir_block_t block,
                           char *out, size_t out_len)
{
    anvil_mir_block_info_t info;
    char block_upper[64];
    if (!anvil_mir_get_block_info(emit->mir, block, &info) || !info.name) {
        snprintf(block_upper, sizeof(block_upper), "B%u", block);
    } else {
        mf_uppercase(block_upper, info.name, sizeof(block_upper));
    }
    snprintf(out, out_len, "%s_%s", emit->func_label, block_upper);
}

static const char *mf_load_op(mf_emit_t *emit, uint16_t size_bits,
                              anvil_mir_reg_class_t reg_class)
{
    if (reg_class == ANVIL_MIR_REG_FPR) return size_bits == 32 ? "LE" : "LD";
    if (size_bits <= 8) return "IC";
    if (size_bits <= 16) return "LH";
    if (size_bits <= 32) return "L";
    return emit->desc->has_64bit_gprs ? "LG" : "L";
}

static const char *mf_store_op(mf_emit_t *emit, uint16_t size_bits,
                               anvil_mir_reg_class_t reg_class)
{
    if (reg_class == ANVIL_MIR_REG_FPR) return size_bits == 32 ? "STE" : "STD";
    if (size_bits <= 8) return "STC";
    if (size_bits <= 16) return "STH";
    if (size_bits <= 32) return "ST";
    return emit->desc->has_64bit_gprs ? "STG" : "ST";
}

static void mf_emit_narrow_load_prefix(mf_emit_t *emit,
                                       const anvil_mir_vreg_info_t *info,
                                       anvil_mir_vreg_t dst)
{
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR ||
        info->size_bits > 8) return;
    anvil_strbuf_appendf(&emit->code, "         %s    %-4s,%s\n",
        emit->desc->has_64bit_gprs ? "XGR" : "XR",
        mf_vreg_reg_name(emit, dst), mf_vreg_reg_name(emit, dst));
}

static void mf_emit_narrow_load_suffix(mf_emit_t *emit,
                                       const anvil_mir_vreg_info_t *info,
                                       anvil_mir_vreg_t dst)
{
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR ||
        info->size_bits > 16) return;
    const char *reg = mf_vreg_reg_name(emit, dst);
    if (info->size_bits <= 8 && info->is_signed) {
        anvil_strbuf_appendf(&emit->code,
            "         SLL   %-4s,24\n         SRA   %-4s,24\n", reg, reg);
    } else if (info->size_bits <= 16 && !info->is_signed) {
        anvil_strbuf_appendf(&emit->code,
            "         N     %-4s,=X'0000FFFF'\n", reg);
    }
    if (emit->desc->has_64bit_gprs) {
        anvil_strbuf_appendf(&emit->code, "         %s %-4s,%s\n",
            info->is_signed ? "LGFR " : "LLGFR", reg, reg);
    }
}

static void mf_emit_copy(mf_emit_t *emit, anvil_mir_vreg_t dst,
                         anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *info = mf_emit_vreg_info(emit, dst);
    if (!info) return;
    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "         %s   %-4s,%s\n",
            info->size_bits == 32 ? "LER" : "LDR",
            mf_vreg_reg_name(emit, dst), mf_vreg_reg_name(emit, src));
    } else {
        anvil_strbuf_appendf(&emit->code, "         %s    %-4s,%s\n",
            emit->desc->has_64bit_gprs ? "LGR" : "LR",
            mf_vreg_reg_name(emit, dst), mf_vreg_reg_name(emit, src));
    }
}

static void mf_emit_unsigned_divmod(mf_emit_t *emit,
                                    anvil_mir_instr_info_t instr,
                                    anvil_mir_vreg_t lhs,
                                    anvil_mir_vreg_t rhs,
                                    bool wide)
{
    const char *divisor = mf_vreg_reg_name(emit, rhs);
    const char *dividend = mf_vreg_reg_name(emit, lhs);
    const char *dst = mf_vreg_reg_name(emit, instr.def);
    anvil_strbuf_appendf(&emit->code, "         %s    R1,%s\n",
                         wide ? "LGR" : "LR", dividend);
    anvil_strbuf_appendf(&emit->code, "         %s   R0,R0\n",
                         wide ? "XGR" : "XR");

    unsigned bits = wide ? 64u : 32u;
    for (unsigned bit = 0; bit < bits; bit++) {
        char carry_done[MF_INTERNAL_LABEL_CAP];
        char no_input[MF_INTERNAL_LABEL_CAP];
        char do_subtract[MF_INTERNAL_LABEL_CAP];
        char no_subtract[MF_INTERNAL_LABEL_CAP];
        snprintf(carry_done, sizeof(carry_done), "%s_UDIV_C_%u_%u",
                 emit->func_label, emit->label_counter, bit);
        snprintf(no_input, sizeof(no_input), "%s_UDIV_I_%u_%u",
                 emit->func_label, emit->label_counter, bit);
        snprintf(do_subtract, sizeof(do_subtract), "%s_UDIV_D_%u_%u",
                 emit->func_label, emit->label_counter, bit);
        snprintf(no_subtract, sizeof(no_subtract), "%s_UDIV_S_%u_%u",
                 emit->func_label, emit->label_counter, bit);
        if (wide) {
            anvil_strbuf_append(&emit->code,
                "         XGR   R14,R14\n"
                "         LTGR  R0,R0\n");
            anvil_strbuf_appendf(&emit->code, "         BNM   %s\n", carry_done);
            anvil_strbuf_append(&emit->code, "         AGHI  R14,1\n");
            anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", carry_done);
            anvil_strbuf_append(&emit->code,
                "         TMHH  R1,X'8000'\n"
                "         SLLG  R0,R0,1\n");
            anvil_strbuf_appendf(&emit->code, "         BZ    %s\n", no_input);
            anvil_strbuf_append(&emit->code, "         AGHI  R0,1\n");
            anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", no_input);
            anvil_strbuf_append(&emit->code, "         SLLG  R1,R1,1\n");
            anvil_strbuf_append(&emit->code, "         LTGR  R14,R14\n");
            anvil_strbuf_appendf(&emit->code, "         BNZ   %s\n", do_subtract);
            anvil_strbuf_appendf(&emit->code, "         CLGR  R0,%s\n", divisor);
            anvil_strbuf_appendf(&emit->code, "         BL    %s\n", no_subtract);
            anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", do_subtract);
            anvil_strbuf_appendf(&emit->code, "         SLGR  R0,%s\n", divisor);
            anvil_strbuf_append(&emit->code, "         AGHI  R1,1\n");
        } else {
            anvil_strbuf_append(&emit->code,
                "         XR    R14,R14\n"
                "         LTR   R0,R0\n");
            anvil_strbuf_appendf(&emit->code, "         BNM   %s\n", carry_done);
            anvil_strbuf_append(&emit->code, "         LA    R14,1\n");
            anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", carry_done);
            anvil_strbuf_append(&emit->code, "         SLDL  R0,1\n");
            anvil_strbuf_append(&emit->code, "         LTR   R14,R14\n");
            anvil_strbuf_appendf(&emit->code, "         BNZ   %s\n", do_subtract);
            anvil_strbuf_appendf(&emit->code, "         CLR   R0,%s\n", divisor);
            anvil_strbuf_appendf(&emit->code, "         BL    %s\n", no_subtract);
            anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", do_subtract);
            anvil_strbuf_appendf(&emit->code, "         SR    R0,%s\n", divisor);
            anvil_strbuf_append(&emit->code, "         LA    R1,1(,R1)\n");
        }
        anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", no_subtract);
    }
    emit->label_counter++;
    anvil_strbuf_appendf(&emit->code, "         %s    %-4s,%s\n",
                         wide ? "LGR" : "LR", dst,
                         instr.op == ANVIL_MIR_OP_UMOD ? "R0" : "R1");
}

static void mf_emit_binary(mf_emit_t *emit, anvil_mir_instr_info_t instr,
                           anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs)
{
    const anvil_mir_vreg_info_t *info = mf_emit_vreg_info(emit, instr.def);
    if (!info) return;
    if (instr.def != lhs) mf_emit_copy(emit, instr.def, lhs);

    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        bool f32 = info->size_bits == 32;
        bool ieee = mf_use_ieee_fp(emit);
        const char *op = NULL;
        switch (instr.op) {
            case ANVIL_MIR_OP_ADD: op = ieee ? (f32 ? "AEBR" : "ADBR") : (f32 ? "AER" : "ADR"); break;
            case ANVIL_MIR_OP_SUB: op = ieee ? (f32 ? "SEBR" : "SDBR") : (f32 ? "SER" : "SDR"); break;
            case ANVIL_MIR_OP_MUL: op = ieee ? (f32 ? "MEEBR" : "MDBR") : (f32 ? "MER" : "MDR"); break;
            case ANVIL_MIR_OP_DIV:
            case ANVIL_MIR_OP_FDIV: op = ieee ? (f32 ? "DEBR" : "DDBR") : (f32 ? "DER" : "DDR"); break;
            default: op = "ADR"; break;
        }
        anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%s\n",
                             op, mf_vreg_reg_name(emit, instr.def),
                             mf_vreg_reg_name(emit, rhs));
        return;
    }

    bool wide = info->size_bits > 32 && emit->desc->has_64bit_gprs;
    const char *op = NULL;
    switch (instr.op) {
        case ANVIL_MIR_OP_ADD: op = wide ? "AGR" : "AR"; break;
        case ANVIL_MIR_OP_SUB: op = wide ? "SGR" : "SR"; break;
        case ANVIL_MIR_OP_MUL:
            if (wide) {
                op = "MSGR";
                break;
            }
            anvil_strbuf_appendf(&emit->code, "         LR    R1,%s\n",
                                 mf_vreg_reg_name(emit, lhs));
            anvil_strbuf_appendf(&emit->code, "         MR    R0,%s\n",
                                 mf_vreg_reg_name(emit, rhs));
            anvil_strbuf_appendf(&emit->code, "         LR    %-4s,R1\n",
                                 mf_vreg_reg_name(emit, instr.def));
            return;
        case ANVIL_MIR_OP_DIV:
        case ANVIL_MIR_OP_SDIV:
        case ANVIL_MIR_OP_UDIV:
        case ANVIL_MIR_OP_SMOD:
        case ANVIL_MIR_OP_UMOD:
            if (instr.op == ANVIL_MIR_OP_UDIV ||
                instr.op == ANVIL_MIR_OP_UMOD) {
                mf_emit_unsigned_divmod(emit, instr, lhs, rhs, wide);
                return;
            }
            if (wide) {
                anvil_strbuf_appendf(&emit->code, "         LGR   R0,%s\n",
                                     mf_vreg_reg_name(emit, lhs));
                anvil_strbuf_append(&emit->code, "         SRDAG R0,R0,64\n");
                anvil_strbuf_appendf(&emit->code, "         DSGR  R0,%s\n",
                                     mf_vreg_reg_name(emit, rhs));
                anvil_strbuf_appendf(&emit->code, "         LGR   %-4s,%s\n",
                                     mf_vreg_reg_name(emit, instr.def),
                                     (instr.op == ANVIL_MIR_OP_SMOD ||
                                      instr.op == ANVIL_MIR_OP_UMOD) ? "R0" : "R1");
            } else {
                anvil_strbuf_appendf(&emit->code, "         LR    R0,%s\n",
                                     mf_vreg_reg_name(emit, lhs));
                anvil_strbuf_append(&emit->code, "         SRDA  R0,32\n");
                anvil_strbuf_appendf(&emit->code, "         DR    R0,%s\n",
                                     mf_vreg_reg_name(emit, rhs));
                anvil_strbuf_appendf(&emit->code, "         LR    %-4s,%s\n",
                                     mf_vreg_reg_name(emit, instr.def),
                                     (instr.op == ANVIL_MIR_OP_SMOD ||
                                      instr.op == ANVIL_MIR_OP_UMOD) ? "R0" : "R1");
            }
            return;
        case ANVIL_MIR_OP_AND: op = wide ? "NGR" : "NR"; break;
        case ANVIL_MIR_OP_OR: op = wide ? "OGR" : "OR"; break;
        case ANVIL_MIR_OP_XOR: op = wide ? "XGR" : "XR"; break;
        default: break;
    }
    if (op) {
        anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%s\n",
                             op, mf_vreg_reg_name(emit, instr.def),
                             mf_vreg_reg_name(emit, rhs));
        return;
    }

    if (instr.op == ANVIL_MIR_OP_SHL || instr.op == ANVIL_MIR_OP_SHR ||
        instr.op == ANVIL_MIR_OP_SAR) {
        const char *shift = instr.op == ANVIL_MIR_OP_SHL
                                ? (wide ? "SLLG" : "SLL")
                                : instr.op == ANVIL_MIR_OP_SHR
                                      ? (wide ? "SRLG" : "SRL")
                                      : (wide ? "SRAG" : "SRA");
        anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,0(%s)\n",
                             shift, mf_vreg_reg_name(emit, instr.def),
                             mf_vreg_reg_name(emit, rhs));
    }
}

static const char *mf_cmp_branch(anvil_mir_opcode_t op)
{
    switch (op) {
        case ANVIL_MIR_OP_CMP_EQ: return "BE";
        case ANVIL_MIR_OP_CMP_NE:
        case ANVIL_MIR_OP_CMP: return "BNE";
        case ANVIL_MIR_OP_CMP_LT:
        case ANVIL_MIR_OP_CMP_ULT: return "BL";
        case ANVIL_MIR_OP_CMP_LE:
        case ANVIL_MIR_OP_CMP_ULE: return "BNH";
        case ANVIL_MIR_OP_CMP_GT:
        case ANVIL_MIR_OP_CMP_UGT: return "BH";
        case ANVIL_MIR_OP_CMP_GE:
        case ANVIL_MIR_OP_CMP_UGE: return "BNL";
        default: return "BNE";
    }
}

static void mf_emit_cmp(mf_emit_t *emit, anvil_mir_instr_info_t instr,
                        anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs)
{
    const anvil_mir_vreg_info_t *lhs_info = mf_emit_vreg_info(emit, lhs);
    const anvil_mir_vreg_info_t *dst_info = mf_emit_vreg_info(emit, instr.def);
    if (!lhs_info || !dst_info) return;

    char true_label[MF_INTERNAL_LABEL_CAP];
    char end_label[MF_INTERNAL_LABEL_CAP];
    snprintf(true_label, sizeof(true_label), "%s_CMP_T_%u",
             emit->func_label, emit->label_counter);
    snprintf(end_label, sizeof(end_label), "%s_CMP_E_%u",
             emit->func_label, emit->label_counter++);

    mf_emit_load_imm(emit, mf_phys_reg(emit, instr.def), 0);
    if (lhs_info->reg_class == ANVIL_MIR_REG_FPR) {
        bool ieee = mf_use_ieee_fp(emit);
        anvil_strbuf_appendf(&emit->code, "         %s   %-4s,%s\n",
            lhs_info->size_bits == 32
                ? (ieee ? "CEBR" : "CER")
                : (ieee ? "CDBR" : "CDR"),
            mf_vreg_reg_name(emit, lhs), mf_vreg_reg_name(emit, rhs));
    } else {
        bool wide = lhs_info->size_bits > 32 && emit->desc->has_64bit_gprs;
        const char *cmp = wide ? "CGR" : "CR";
        if (instr.op == ANVIL_MIR_OP_CMP_ULT ||
            instr.op == ANVIL_MIR_OP_CMP_ULE ||
            instr.op == ANVIL_MIR_OP_CMP_UGT ||
            instr.op == ANVIL_MIR_OP_CMP_UGE) {
            cmp = wide ? "CLGR" : "CLR";
        }
        anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%s\n",
                             cmp, mf_vreg_reg_name(emit, lhs),
                             mf_vreg_reg_name(emit, rhs));
    }
    if (instr.op == ANVIL_MIR_OP_FCMP) {
        static const unsigned masks[] = {
            0, 8, 2, 10, 4, 12, 6, 14,
            9, 3, 11, 5, 13, 7, 1, 15
        };
        unsigned pred = (unsigned)instr.imm;
        if (pred > ANVIL_FCMP_TRUE) return;
        if (masks[pred])
            anvil_strbuf_appendf(&emit->code, "         BC    %u,%s\n",
                                 masks[pred], true_label);
    } else {
        anvil_strbuf_appendf(&emit->code, "         %-5s %s\n",
                             mf_cmp_branch(instr.op), true_label);
    }
    anvil_strbuf_appendf(&emit->code, "         B     %s\n", end_label);
    anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", true_label);
    mf_emit_load_imm(emit, mf_phys_reg(emit, instr.def), 1);
    anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", end_label);
}

static void mf_emit_unary(mf_emit_t *emit, anvil_mir_instr_info_t instr,
                          anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *info = mf_emit_vreg_info(emit, instr.def);
    if (!info) return;
    if (instr.def != src) mf_emit_copy(emit, instr.def, src);

    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        if (instr.op == ANVIL_MIR_OP_NEG) {
            anvil_strbuf_appendf(&emit->code, "         %s   %-4s,%s\n",
                info->size_bits == 32 ? "LCER" : "LCDR",
                mf_vreg_reg_name(emit, instr.def), mf_vreg_reg_name(emit, instr.def));
        } else if (instr.op == ANVIL_MIR_OP_FABS) {
            anvil_strbuf_appendf(&emit->code, "         %s   %-4s,%s\n",
                info->size_bits == 32 ? "LPER" : "LPDR",
                mf_vreg_reg_name(emit, instr.def), mf_vreg_reg_name(emit, instr.def));
        }
        return;
    }

    switch (instr.op) {
        case ANVIL_MIR_OP_NEG:
            anvil_strbuf_appendf(&emit->code, "         %s    %-4s,%s\n",
                emit->desc->has_64bit_gprs ? "LCGR" : "LCR",
                mf_vreg_reg_name(emit, instr.def), mf_vreg_reg_name(emit, instr.def));
            break;
        case ANVIL_MIR_OP_NOT:
            anvil_strbuf_appendf(&emit->code, "         %s    %-4s,=X'%s'\n",
                emit->desc->has_64bit_gprs ? "XG" : "X",
                mf_vreg_reg_name(emit, instr.def),
                emit->desc->has_64bit_gprs
                    ? "FFFFFFFFFFFFFFFF" : "FFFFFFFF");
            break;
        default:
            break;
    }
}

static void mf_emit_signed_int_to_fp(mf_emit_t *emit, const char *dst,
                                     const char *src, uint16_t src_bits,
                                     uint16_t dst_bits)
{
    if (!mf_use_ieee_fp(emit) &&
        (emit->desc->variant == ANVIL_MAINFRAME_VARIANT_S370 ||
         emit->desc->variant == ANVIL_MAINFRAME_VARIANT_S370_XA)) {
        char positive[MF_INTERNAL_LABEL_CAP];
        snprintf(positive, sizeof(positive), "%s_IHFP_P_%u",
                 emit->func_label, emit->label_counter++);
        anvil_strbuf_appendf(&emit->code,
            "         LR    R0,%s\n"
            "         XR    R14,R14\n"
            "         LTR   R0,R0\n"
            "         BNM   %s\n"
            "         LCR   R0,R0\n"
            "         L     R14,=X'80000000'\n"
            "%-8s DS    0H\n"
            "         L     R1,=X'4E000000'\n"
            "         OR    R1,R14\n"
            "         ST    R1,%u(,R11)\n"
            "         ST    R0,%u(,R11)\n"
            "         LD    %-4s,%u(,R11)\n",
            src, positive, positive, emit->desc->fp_temp_offset,
            emit->desc->fp_temp_offset + 4, dst, emit->desc->fp_temp_offset);
        if (dst_bits == 32) {
            anvil_strbuf_appendf(&emit->code,
                "         LEDR  %-4s,%s\n", dst, dst);
        }
        return;
    }
    const char *op;
    if (mf_use_ieee_fp(emit)) {
        if (src_bits > 32) op = dst_bits == 32 ? "CEGBR" : "CDGBR";
        else op = dst_bits == 32 ? "CEFBR" : "CDFBR";
    } else {
        if (src_bits > 32) op = dst_bits == 32 ? "CEGR" : "CDGR";
        else op = dst_bits == 32 ? "CEFR" : "CDFR";
    }
    anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%s\n", op, dst, src);
}

static void mf_emit_fp_to_signed_int(mf_emit_t *emit, const char *dst,
                                     const char *src, uint16_t src_bits,
                                     uint16_t dst_bits)
{
    if (!mf_use_ieee_fp(emit) &&
        (emit->desc->variant == ANVIL_MAINFRAME_VARIANT_S370 ||
         emit->desc->variant == ANVIL_MAINFRAME_VARIANT_S370_XA)) {
        char less32[MF_INTERNAL_LABEL_CAP];
        char zero[MF_INTERNAL_LABEL_CAP];
        char apply_sign[MF_INTERNAL_LABEL_CAP];
        char end[MF_INTERNAL_LABEL_CAP];
        unsigned label = emit->label_counter++;
        snprintf(less32, sizeof(less32), "%s_HFI_L_%u", emit->func_label, label);
        snprintf(zero, sizeof(zero), "%s_HFI_Z_%u", emit->func_label, label);
        snprintf(apply_sign, sizeof(apply_sign), "%s_HFI_S_%u", emit->func_label, label);
        snprintf(end, sizeof(end), "%s_HFI_E_%u", emit->func_label, label);
        anvil_strbuf_appendf(&emit->code, "         %s  F0,%s\n",
                             src_bits == 32 ? "LDER" : "LDR", src);
        anvil_strbuf_appendf(&emit->code,
            "         STD   F0,%u(,R11)\n"
            "         L     R0,%u(,R11)\n"
            "         LR    R14,R0\n"
            "         SRL   R0,24\n"
            "         N     R0,=X'0000007F'\n"
            "         SLL   R0,2\n"
            "         LCR   R0,R0\n"
            "         A     R0,=F'312'\n"
            "         L     %-4s,%u(,R11)\n"
            "         N     %-4s,=X'00FFFFFF'\n"
            "         C     R0,=F'56'\n"
            "         BNL   %s\n"
            "         C     R0,=F'32'\n"
            "         BL    %s\n"
            "         S     R0,=F'32'\n"
            "         SRL   %-4s,0(R0)\n"
            "         B     %s\n"
            "%-8s DS    0H\n"
            "         L     R1,%u(,R11)\n"
            "         SRL   R1,0(R0)\n"
            "         LCR   R0,R0\n"
            "         A     R0,=F'32'\n"
            "         SLL   %-4s,0(R0)\n"
            "         OR    %-4s,R1\n"
            "         B     %s\n"
            "%-8s DS    0H\n"
            "         XR    %-4s,%s\n"
            "%-8s DS    0H\n"
            "         LTR   R14,R14\n"
            "         BNM   %s\n"
            "         LCR   %-4s,%s\n"
            "%-8s DS    0H\n",
            emit->desc->fp_temp_offset, emit->desc->fp_temp_offset,
            dst, emit->desc->fp_temp_offset, dst, zero, less32,
            dst, apply_sign, less32, emit->desc->fp_temp_offset + 4,
            dst, dst, apply_sign, zero, dst, dst, apply_sign, end,
            dst, dst, end);
        (void)dst_bits;
        return;
    }
    const char *op;
    if (mf_use_ieee_fp(emit)) {
        if (dst_bits > 32) op = src_bits == 32 ? "CGEBR" : "CGDBR";
        else op = src_bits == 32 ? "CFEBR" : "CFDBR";
        /* M3=5 is round toward zero.  The optional M4 field is omitted,
           selecting the architectural default exception behavior. */
        anvil_strbuf_appendf(&emit->code,
            "         %-5s %-4s,5,%s\n", op, dst, src);
    } else {
        if (dst_bits > 32) op = src_bits == 32 ? "CGER" : "CGDR";
        else op = src_bits == 32 ? "CFER" : "CFDR";
        anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,5,%s\n",
                             op, dst, src);
    }
}

static void mf_emit_integer_normalize(mf_emit_t *emit, const char *reg,
                                      uint16_t bits, bool is_signed)
{
    if (bits <= 8) {
        if (is_signed) {
            anvil_strbuf_appendf(&emit->code,
                "         SLL   %-4s,24\n         SRA   %-4s,24\n", reg, reg);
        } else {
            anvil_strbuf_appendf(&emit->code,
                "         N     %-4s,=X'000000FF'\n", reg);
        }
    } else if (bits <= 16) {
        if (is_signed) {
            anvil_strbuf_appendf(&emit->code,
                "         SLL   %-4s,16\n         SRA   %-4s,16\n", reg, reg);
        } else {
            anvil_strbuf_appendf(&emit->code,
                "         N     %-4s,=X'0000FFFF'\n", reg);
        }
    }
    if (emit->desc->has_64bit_gprs && bits <= 32) {
        anvil_strbuf_appendf(&emit->code, "         %s %-4s,%s\n",
            is_signed ? "LGFR " : "LLGFR", reg, reg);
    }
}

static void mf_emit_numeric_cast(mf_emit_t *emit,
                                 anvil_mir_instr_info_t instr,
                                 anvil_mir_vreg_t src,
                                 const anvil_mir_vreg_info_t *dst_info,
                                 const anvil_mir_vreg_info_t *src_info)
{
    const char *dst = mf_vreg_reg_name(emit, instr.def);
    const char *source = mf_vreg_reg_name(emit, src);
    bool ieee = mf_use_ieee_fp(emit);

    if (instr.op == ANVIL_MIR_OP_FPEXT ||
        instr.op == ANVIL_MIR_OP_FPTRUNC) {
        const char *op = instr.op == ANVIL_MIR_OP_FPEXT
            ? (ieee ? "LDEBR" : "LDER")
            : (ieee ? "LEDBR" : "LEDR");
        anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%s\n",
                             op, dst, source);
        return;
    }

    if (instr.op == ANVIL_MIR_OP_SITOFP ||
        instr.op == ANVIL_MIR_OP_UITOFP) {
        bool is_unsigned = instr.op == ANVIL_MIR_OP_UITOFP;
        const char *integer = source;
        if (src_info->size_bits < (emit->desc->has_64bit_gprs ? 64 : 32)) {
            anvil_strbuf_appendf(&emit->code, "         %s    R0,%s\n",
                emit->desc->has_64bit_gprs ? "LGR" : "LR", source);
            mf_emit_integer_normalize(emit, "R0", src_info->size_bits,
                                      !is_unsigned);
            integer = "R0";
            /* A zero-extended u8/u16/u32 fits the wider signed domain. */
            is_unsigned = false;
        }
        if (!is_unsigned) {
            mf_emit_signed_int_to_fp(emit, dst, integer,
                                     src_info->size_bits, dst_info->size_bits);
            return;
        }

        char low_label[MF_INTERNAL_LABEL_CAP];
        char end_label[MF_INTERNAL_LABEL_CAP];
        snprintf(low_label, sizeof(low_label), "%s_UIFP_L_%u",
                 emit->func_label, emit->label_counter);
        snprintf(end_label, sizeof(end_label), "%s_UIFP_E_%u",
                 emit->func_label, emit->label_counter++);
        anvil_strbuf_appendf(&emit->code, "         %s  %s,%s\n",
            emit->desc->has_64bit_gprs ? "LTGR" : "LTR", source, source);
        anvil_strbuf_appendf(&emit->code, "         BNM   %s\n", low_label);
        anvil_strbuf_appendf(&emit->code, "         %s    R0,%s\n",
            emit->desc->has_64bit_gprs ? "LGR" : "LR", source);
        if (emit->desc->has_64bit_gprs) {
            anvil_strbuf_append(&emit->code, "         SRLG  R0,R0,1\n");
        } else {
            anvil_strbuf_append(&emit->code, "         SRL   R0,1\n");
        }
        anvil_strbuf_appendf(&emit->code, "         %s    R1,%s\n",
            emit->desc->has_64bit_gprs ? "LGR" : "LR", source);
        anvil_strbuf_append(&emit->code,
            emit->desc->has_64bit_gprs
                ? "         NILL  R1,X'0001'\n"
                : "         N     R1,=X'00000001'\n");
        anvil_strbuf_appendf(&emit->code, "         %s    R0,R1\n",
            emit->desc->has_64bit_gprs ? "OGR" : "OR");
        mf_emit_signed_int_to_fp(emit, dst, "R0",
                                 src_info->size_bits, dst_info->size_bits);
        anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%s\n",
            ieee ? (dst_info->size_bits == 32 ? "AEBR" : "ADBR")
                 : (dst_info->size_bits == 32 ? "AER" : "ADR"), dst, dst);
        anvil_strbuf_appendf(&emit->code, "         B     %s\n", end_label);
        anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", low_label);
        mf_emit_signed_int_to_fp(emit, dst, source,
                                 src_info->size_bits, dst_info->size_bits);
        anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", end_label);
        return;
    }

    bool is_unsigned = instr.op == ANVIL_MIR_OP_FPTOUI;
    if (!is_unsigned || dst_info->size_bits < (emit->desc->has_64bit_gprs ? 64 : 32)) {
        mf_emit_fp_to_signed_int(emit, dst, source,
                                 src_info->size_bits, dst_info->size_bits);
        mf_emit_integer_normalize(emit, dst, dst_info->size_bits, !is_unsigned);
        return;
    }

    char low_label[MF_INTERNAL_LABEL_CAP];
    char end_label[MF_INTERNAL_LABEL_CAP];
    snprintf(low_label, sizeof(low_label), "%s_FPUI_L_%u",
             emit->func_label, emit->label_counter);
    snprintf(end_label, sizeof(end_label), "%s_FPUI_E_%u",
             emit->func_label, emit->label_counter++);
    const char *literal;
    if (src_info->size_bits == 32) {
        literal = ieee
            ? (dst_info->size_bits > 32
                ? "=EB'9223372036854775808'" : "=EB'2147483648'")
            : (dst_info->size_bits > 32
                ? "=E'9223372036854775808'" : "=E'2147483648'");
    } else {
        literal = ieee
            ? (dst_info->size_bits > 32
                ? "=DB'9223372036854775808'" : "=DB'2147483648'")
            : (dst_info->size_bits > 32
                ? "=D'9223372036854775808'" : "=D'2147483648'");
    }
    anvil_strbuf_appendf(&emit->code, "         %s    F0,%s\n",
        src_info->size_bits == 32 ? "LE" : "LD", literal);
    anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,F0\n",
        ieee ? (src_info->size_bits == 32 ? "CEBR" : "CDBR")
             : (src_info->size_bits == 32 ? "CER" : "CDR"), source);
    anvil_strbuf_appendf(&emit->code, "         BL    %s\n", low_label);
    anvil_strbuf_appendf(&emit->code, "         %-5s F0,%s\n",
        ieee ? (src_info->size_bits == 32 ? "SEBR" : "SDBR")
             : (src_info->size_bits == 32 ? "SER" : "SDR"), source);
    anvil_strbuf_appendf(&emit->code, "         %-5s F0,F0\n",
        ieee ? (src_info->size_bits == 32 ? "LCEBR" : "LCDBR")
             : (src_info->size_bits == 32 ? "LCER" : "LCDR"));
    mf_emit_fp_to_signed_int(emit, dst, "F0",
                             src_info->size_bits, dst_info->size_bits);
    anvil_strbuf_appendf(&emit->code, "         %s    %-4s,=X'%s'\n",
        emit->desc->has_64bit_gprs ? "OG" : "O", dst,
        emit->desc->has_64bit_gprs ? "8000000000000000" : "80000000");
    anvil_strbuf_appendf(&emit->code, "         B     %s\n", end_label);
    anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", low_label);
    mf_emit_fp_to_signed_int(emit, dst, source,
                             src_info->size_bits, dst_info->size_bits);
    anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", end_label);
}

static void mf_emit_cast(mf_emit_t *emit, anvil_mir_instr_info_t instr,
                         anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *dst_info = mf_emit_vreg_info(emit, instr.def);
    const anvil_mir_vreg_info_t *src_info = mf_emit_vreg_info(emit, src);
    if (!dst_info || !src_info) return;
    bool numeric_fp_cast = instr.op == ANVIL_MIR_OP_SITOFP ||
                           instr.op == ANVIL_MIR_OP_UITOFP ||
                           instr.op == ANVIL_MIR_OP_FPTOSI ||
                           instr.op == ANVIL_MIR_OP_FPTOUI ||
                           instr.op == ANVIL_MIR_OP_FPEXT ||
                           instr.op == ANVIL_MIR_OP_FPTRUNC;
    if (!numeric_fp_cast && dst_info->reg_class == src_info->reg_class) {
        mf_emit_copy(emit, instr.def, src);
        if (dst_info->reg_class == ANVIL_MIR_REG_GPR &&
            instr.op == ANVIL_MIR_OP_ZEXT && src_info->size_bits < dst_info->size_bits) {
            if (src_info->size_bits == 8) {
                anvil_strbuf_appendf(&emit->code, "         N     %-4s,=X'000000FF'\n",
                                     mf_vreg_reg_name(emit, instr.def));
            } else if (src_info->size_bits == 16) {
                anvil_strbuf_appendf(&emit->code, "         N     %-4s,=X'0000FFFF'\n",
                                     mf_vreg_reg_name(emit, instr.def));
            }
        }
        return;
    }
    if (numeric_fp_cast) {
        mf_emit_numeric_cast(emit, instr, src, dst_info, src_info);
    }
}

static void mf_emit_mov(mf_emit_t *emit, anvil_mir_instr_info_t instr)
{
    const anvil_mir_vreg_info_t *info = mf_emit_vreg_info(emit, instr.def);
    if (!info || !instr.has_imm) return;
    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        mf_emit_load_fp_imm(emit, mf_phys_reg(emit, instr.def),
                            info->size_bits, instr.imm);
    } else {
        mf_emit_load_imm(emit, mf_phys_reg(emit, instr.def), instr.imm);
    }
}

static void mf_emit_symbol_addr(mf_emit_t *emit, anvil_mir_instr_info_t instr)
{
    char upper[96];
    mf_uppercase(upper, instr.symbol, sizeof(upper));
    if (emit->desc->has_64bit_gprs) {
        anvil_strbuf_appendf(&emit->code, "         LARL  %-4s,%s\n",
                             mf_vreg_reg_name(emit, instr.def), upper);
    } else {
        anvil_strbuf_appendf(&emit->code, "         LA    %-4s,%s\n",
                             mf_vreg_reg_name(emit, instr.def), upper);
    }
}

static bool mf_get_uses(const anvil_mir_func_t *mir, size_t index,
                        anvil_mir_vreg_t *u0, anvil_mir_vreg_t *u1,
                        anvil_mir_vreg_t *u2)
{
    anvil_mir_instr_info_t info;
    if (!anvil_mir_get_instr_info(mir, index, &info)) return false;
    if (info.num_uses > 0 && u0) *u0 = anvil_mir_get_instr_use(mir, index, 0);
    if (info.num_uses > 1 && u1) *u1 = anvil_mir_get_instr_use(mir, index, 1);
    if (info.num_uses > 2 && u2) *u2 = anvil_mir_get_instr_use(mir, index, 2);
    return true;
}

static void mf_emit_load(mf_emit_t *emit, anvil_mir_instr_info_t instr,
                         size_t index)
{
    anvil_mir_vreg_t addr = ANVIL_MIR_NO_VREG;
    mf_get_uses(emit->mir, index, &addr, NULL, NULL);
    const anvil_mir_vreg_info_t *info = mf_emit_vreg_info(emit, instr.def);
    if (!info) return;
    mf_emit_narrow_load_prefix(emit, info, instr.def);
    anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,0(,%s)\n",
        mf_load_op(emit, info->size_bits, info->reg_class),
        mf_vreg_reg_name(emit, instr.def), mf_vreg_reg_name(emit, addr));
    mf_emit_narrow_load_suffix(emit, info, instr.def);
}

static void mf_emit_store(mf_emit_t *emit, anvil_mir_instr_info_t instr,
                          size_t index)
{
    (void)instr;
    anvil_mir_vreg_t value = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t addr = ANVIL_MIR_NO_VREG;
    mf_get_uses(emit->mir, index, &value, &addr, NULL);
    const anvil_mir_vreg_info_t *info = mf_emit_vreg_info(emit, value);
    if (!info) return;
    anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,0(,%s)\n",
        mf_store_op(emit, info->size_bits, info->reg_class),
        mf_vreg_reg_name(emit, value), mf_vreg_reg_name(emit, addr));
}

static void mf_emit_frame_addr(mf_emit_t *emit, anvil_mir_instr_info_t instr)
{
    int off = instr.frame_slot >= 0 ? emit->frame_offsets[instr.frame_slot] : 0;
    anvil_strbuf_appendf(&emit->code, "         LA    %-4s,%d(,R11)\n",
                         mf_vreg_reg_name(emit, instr.def), off);
}

static void mf_emit_incoming_arg(mf_emit_t *emit, anvil_mir_instr_info_t instr)
{
    const anvil_mir_vreg_info_t *info = mf_emit_vreg_info(emit, instr.def);
    if (!info || !instr.has_imm) return;
    if (emit->desc->has_64bit_gprs) {
        anvil_strbuf_appendf(&emit->code, "         LG    R0,%lld(,R1)\n",
                             (long long)instr.imm);
        anvil_strbuf_append(&emit->code, "         NIHH  R0,X'7FFF'\n");
    } else {
        anvil_strbuf_appendf(&emit->code, "         L     R0,%lld(,R1)\n",
                             (long long)instr.imm);
        anvil_strbuf_append(&emit->code, "         N     R0,=X'7FFFFFFF'\n");
    }
    mf_emit_narrow_load_prefix(emit, info, instr.def);
    anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,0(,%s)\n",
        mf_load_op(emit, info->size_bits, info->reg_class),
        mf_vreg_reg_name(emit, instr.def), "R0");
    mf_emit_narrow_load_suffix(emit, info, instr.def);
}

static void mf_emit_call_stack_arg(mf_emit_t *emit, anvil_mir_instr_info_t instr,
                                   size_t index)
{
    if (!instr.has_imm || instr.imm < 0 || emit->outgoing_values_offset < 0) return;
    anvil_mir_vreg_t value = ANVIL_MIR_NO_VREG;
    mf_get_uses(emit->mir, index, &value, NULL, NULL);
    const anvil_mir_vreg_info_t *info = mf_emit_vreg_info(emit, value);
    if (!info) return;

    if (!emit->call_arg_value_offsets ||
        emit->call_arg_value_offsets[index] < 0) return;
    int value_off = emit->outgoing_values_offset +
                    emit->call_arg_value_offsets[index];
    int list_off = emit->outgoing_param_list_offset +
                   (int)instr.imm * (int)emit->desc->ptr_size;

    anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%d(,R11)\n",
        mf_store_op(emit, info->size_bits, info->reg_class),
        mf_vreg_reg_name(emit, value), value_off);
    anvil_strbuf_appendf(&emit->code, "         LA    R0,%d(,R11)\n", value_off);
    if (emit->call_arg_is_last && emit->call_arg_is_last[index]) {
        if (emit->desc->has_64bit_gprs) {
            anvil_strbuf_append(&emit->code, "         OIHH  R0,X'8000'\n");
        } else {
            anvil_strbuf_append(&emit->code, "         O     R0,=X'80000000'\n");
        }
    }
    anvil_strbuf_appendf(&emit->code, "         %s    R0,%d(,R11)\n",
                         emit->desc->has_64bit_gprs ? "STG" : "ST", list_off);
}

static void mf_emit_call(mf_emit_t *emit, anvil_mir_instr_info_t instr,
                         size_t index)
{
    (void)index;
    if (emit->outgoing_param_list_offset >= 0 &&
        emit->call_arg_counts && emit->call_arg_counts[index] > 0) {
        anvil_strbuf_appendf(&emit->code, "         LA    R1,%d(,R11)\n",
                             emit->outgoing_param_list_offset);
    } else {
        anvil_strbuf_appendf(&emit->code, "         %s    R1,R1\n",
                             emit->desc->has_64bit_gprs ? "XGR" : "XR");
    }
    if (instr.symbol) {
        char upper[96];
        mf_uppercase(upper, instr.symbol, sizeof(upper));
        if (emit->desc->has_64bit_gprs) {
            anvil_strbuf_appendf(&emit->code, "         LARL  R15,%s\n", upper);
        } else {
            anvil_strbuf_appendf(&emit->code, "         L     R15,=V(%s)\n", upper);
        }
    } else if (instr.num_uses > 0) {
        anvil_mir_vreg_t callee = anvil_mir_get_instr_use(emit->mir, index, 0);
        anvil_strbuf_appendf(&emit->code, "         %s    R15,%s\n",
                             emit->desc->has_64bit_gprs ? "LGR" : "LR",
                             mf_vreg_reg_name(emit, callee));
    }
    anvil_strbuf_append(&emit->code, "         BALR  R14,R15\n");
}

static void mf_emit_ret(mf_emit_t *emit, anvil_mir_instr_info_t instr,
                        size_t index)
{
    if (instr.num_uses > 0) {
        anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, index, 0);
        const anvil_mir_vreg_info_t *info = mf_emit_vreg_info(emit, value);
        if (info && info->reg_class == ANVIL_MIR_REG_GPR &&
            mf_phys_reg(emit, value) != emit->desc->return_gpr) {
            anvil_strbuf_appendf(&emit->code, "         %s    R15,%s\n",
                emit->desc->has_64bit_gprs ? "LGR" : "LR",
                mf_vreg_reg_name(emit, value));
        }
    }
    mf_emit_epilogue(emit);
}

static void mf_emit_dynamic_alloca(mf_emit_t *emit,
                                   anvil_mir_instr_info_t instr,
                                   anvil_mir_vreg_t count)
{
    const char *dst = mf_vreg_reg_name(emit, instr.def);
    const char *src = mf_vreg_reg_name(emit, count);
    long long elem_size = (long long)instr.imm;
    unsigned local = emit->desc->local_area_offset;

    if (emit->desc->has_64bit_gprs) {
        anvil_strbuf_appendf(&emit->code, "         LGR   R0,%s\n", src);
        if (elem_size != 1) {
            anvil_strbuf_appendf(&emit->code,
                "         MSGFI R0,%lld\n", elem_size);
        }
        anvil_strbuf_appendf(&emit->code,
            "         LGR   %-4s,R13\n"
            "         AGHI  %-4s,%u\n"
            "         AGHI  %-4s,15\n"
            "         NILL  %-4s,X'FFF0'\n",
            dst, dst, local, dst, dst);
        anvil_strbuf_appendf(&emit->code,
            "         AGR   R0,%s\n"
            "         AGHI  R0,15\n"
            "         NILL  R0,X'FFF0'\n"
            "         STG   R11,8(,R0)\n"
            "         STG   R0,16(,R11)\n"
            "         LGR   R13,R0\n", dst);
    } else {
        anvil_strbuf_appendf(&emit->code, "         LR    R1,%s\n", src);
        if (elem_size != 1) {
            anvil_strbuf_appendf(&emit->code,
                "         M     R0,=F'%lld'\n", elem_size);
        }
        anvil_strbuf_append(&emit->code, "         LR    R0,R1\n");
        anvil_strbuf_appendf(&emit->code,
            "         LR    %-4s,R13\n"
            "         LA    %-4s,%u(,%s)\n"
            "         LA    %-4s,15(,%s)\n"
            "         N     %-4s,=X'FFFFFFF0'\n",
            dst, dst, local, dst, dst, dst, dst);
        anvil_strbuf_appendf(&emit->code,
            "         AR    R0,%s\n"
            "         LA    R0,15(,R0)\n"
            "         N     R0,=X'FFFFFFF0'\n"
            "         ST    R11,4(,R0)\n"
            "         ST    R0,8(,R11)\n"
            "         LR    R13,R0\n", dst);
    }
}

static void mf_emit_branch_target(mf_emit_t *emit, anvil_mir_block_t block)
{
    char label[MF_INTERNAL_LABEL_CAP];
    mf_block_label(emit, block, label, sizeof(label));
    anvil_strbuf_appendf(&emit->code, "%s", label);
}

static void mf_emit_instr(mf_emit_t *emit, anvil_mir_instr_info_t instr,
                          size_t index)
{
    anvil_mir_vreg_t u0 = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t u1 = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t u2 = ANVIL_MIR_NO_VREG;
    mf_get_uses(emit->mir, index, &u0, &u1, &u2);

    switch (instr.op) {
        case ANVIL_MIR_OP_MOV:
            mf_emit_mov(emit, instr);
            break;
        case ANVIL_MIR_OP_COPY:
        case ANVIL_MIR_OP_BITCAST:
            mf_emit_copy(emit, instr.def, u0);
            break;
        case ANVIL_MIR_OP_ADD:
        case ANVIL_MIR_OP_SUB:
        case ANVIL_MIR_OP_MUL:
        case ANVIL_MIR_OP_DIV:
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
        case ANVIL_MIR_OP_FDIV:
            mf_emit_binary(emit, instr, u0, u1);
            break;
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
            mf_emit_cmp(emit, instr, u0, u1);
            break;
        case ANVIL_MIR_OP_NEG:
        case ANVIL_MIR_OP_NOT:
        case ANVIL_MIR_OP_FABS:
            mf_emit_unary(emit, instr, u0);
            break;
        case ANVIL_MIR_OP_ZEXT:
        case ANVIL_MIR_OP_SEXT:
        case ANVIL_MIR_OP_TRUNC:
        case ANVIL_MIR_OP_SITOFP:
        case ANVIL_MIR_OP_UITOFP:
        case ANVIL_MIR_OP_FPTOSI:
        case ANVIL_MIR_OP_FPTOUI:
        case ANVIL_MIR_OP_FPEXT:
        case ANVIL_MIR_OP_FPTRUNC:
            mf_emit_cast(emit, instr, u0);
            break;
        case ANVIL_MIR_OP_SYMBOL_ADDR:
            mf_emit_symbol_addr(emit, instr);
            break;
        case ANVIL_MIR_OP_LOAD:
            mf_emit_load(emit, instr, index);
            break;
        case ANVIL_MIR_OP_STORE:
            mf_emit_store(emit, instr, index);
            break;
        case ANVIL_MIR_OP_FRAME_ADDR:
            mf_emit_frame_addr(emit, instr);
            break;
        case ANVIL_MIR_OP_INCOMING_STACK_ARG:
            mf_emit_incoming_arg(emit, instr);
            break;
        case ANVIL_MIR_OP_CALL_STACK_ARG:
            mf_emit_call_stack_arg(emit, instr, index);
            break;
        case ANVIL_MIR_OP_CALL:
            mf_emit_call(emit, instr, index);
            break;
        case ANVIL_MIR_OP_BR:
            anvil_strbuf_append(&emit->code, "         B     ");
            mf_emit_branch_target(emit, instr.true_block);
            anvil_strbuf_append(&emit->code, "\n");
            break;
        case ANVIL_MIR_OP_BR_COND:
            anvil_strbuf_appendf(&emit->code, "         %s    R0,%s\n",
                emit->desc->has_64bit_gprs ? "LGR" : "LR", mf_vreg_reg_name(emit, u0));
            anvil_strbuf_appendf(&emit->code, "         %s  R0,R0\n",
                                 emit->desc->has_64bit_gprs ? "LTGR" : "LTR");
            anvil_strbuf_append(&emit->code, "         BNE   ");
            mf_emit_branch_target(emit, instr.true_block);
            anvil_strbuf_append(&emit->code, "\n         B     ");
            mf_emit_branch_target(emit, instr.false_block);
            anvil_strbuf_append(&emit->code, "\n");
            break;
        case ANVIL_MIR_OP_RET:
            mf_emit_ret(emit, instr, index);
            break;
        case ANVIL_MIR_OP_SPILL_LOAD: {
            const anvil_mir_vreg_info_t *info = mf_emit_vreg_info(emit, instr.def);
            int off = instr.spill_slot >= 0 ? emit->spill_offsets[instr.spill_slot] : 0;
            if (info) {
                mf_emit_narrow_load_prefix(emit, info, instr.def);
                anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%d(,R11)\n",
                    mf_load_op(emit, info->size_bits, info->reg_class),
                    mf_vreg_reg_name(emit, instr.def), off);
                mf_emit_narrow_load_suffix(emit, info, instr.def);
            }
            break;
        }
        case ANVIL_MIR_OP_SPILL_STORE: {
            const anvil_mir_vreg_info_t *info = mf_emit_vreg_info(emit, u0);
            int off = instr.spill_slot >= 0 ? emit->spill_offsets[instr.spill_slot] : 0;
            if (info) {
                anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%d(,R11)\n",
                    mf_store_op(emit, info->size_bits, info->reg_class),
                    mf_vreg_reg_name(emit, u0), off);
            }
            break;
        }
        case ANVIL_MIR_OP_DYN_ALLOCA:
            mf_emit_dynamic_alloca(emit, instr, u0);
            break;
        case ANVIL_MIR_OP_SELECT: {
            char true_label[MF_INTERNAL_LABEL_CAP];
            char end_label[MF_INTERNAL_LABEL_CAP];
            snprintf(true_label, sizeof(true_label), "%s_SEL_T_%u",
                     emit->func_label, emit->label_counter);
            snprintf(end_label, sizeof(end_label), "%s_SEL_E_%u",
                     emit->func_label, emit->label_counter++);
            anvil_strbuf_appendf(&emit->code, "         %s    R0,%s\n",
                emit->desc->has_64bit_gprs ? "LGR" : "LR",
                mf_vreg_reg_name(emit, u0));
            anvil_strbuf_appendf(&emit->code, "         %s  R0,R0\n",
                                 emit->desc->has_64bit_gprs ? "LTGR" : "LTR");
            anvil_strbuf_appendf(&emit->code, "         BNE   %s\n", true_label);
            mf_emit_copy(emit, instr.def, u2);
            anvil_strbuf_appendf(&emit->code, "         B     %s\n", end_label);
            anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", true_label);
            mf_emit_copy(emit, instr.def, u1);
            anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", end_label);
            break;
        }
        case ANVIL_MIR_OP_KEEPALIVE:
            break;
        case ANVIL_MIR_OP_CALL_RESULT:
        case ANVIL_MIR_OP_RET_VALUE_PART:
        case ANVIL_MIR_OP_INVALID:
            break;
    }
}

static void mf_emit_string_literals(mf_emit_t *emit)
{
    for (size_t i = 0; i < anvil_mir_num_string_literals(emit->mir); i++) {
        anvil_mir_string_literal_info_t info;
        if (!anvil_mir_get_string_literal_info(emit->mir, i, &info)) continue;
        char upper[96];
        mf_uppercase(upper, info.label, sizeof(upper));
        anvil_strbuf_appendf(&emit->code, "%-8s DC    C'", upper);
        const unsigned char *p = (const unsigned char *)(info.value ? info.value : "");
        while (*p) {
            if (*p == '\'') {
                anvil_strbuf_append(&emit->code, "''");
            } else if (*p >= 32 && *p < 127) {
                anvil_strbuf_append_char(&emit->code, (char)*p);
            }
            p++;
        }
        anvil_strbuf_append(&emit->code, "'\n");
        anvil_strbuf_appendf(&emit->code, "         DC    X'00'\n");
    }
}

static bool mf_emit_mir_ex(const anvil_mir_func_t *mir,
                           anvil_mainframe_variant_t variant,
                           anvil_fp_format_t fp_format,
                           char **output,
                           size_t *len)
{
    const anvil_mainframe_target_desc_t *desc =
        anvil_mainframe_get_target_desc(variant);
    if (!desc || !mir || !output) return false;
    *output = NULL;
    if (len) *len = 0;
    if (!anvil_mainframe_verify_mir_legal(mir, variant, NULL, 0)) return false;

    mf_emit_t emit;
    memset(&emit, 0, sizeof(emit));
    emit.desc = desc;
    emit.mir = mir;
    emit.fp_format = fp_format;
    anvil_strbuf_init(&emit.code);
    const char *mir_name = anvil_mir_func_name(mir);
    if (mir_name && strlen(mir_name) >= sizeof(emit.func_label)) {
        anvil_strbuf_destroy(&emit.code);
        return false;
    }
    mf_uppercase(emit.func_label, mir_name, sizeof(emit.func_label));

    bool ok = mf_prepare_frame(&emit);
    if (ok) {
        mf_emit_header(&emit);
        mf_emit_prologue(&emit);
        for (size_t b = 0;
             b < anvil_mir_num_blocks(mir) && !emit.code.failed; b++) {
            if (b != 0) {
                char label[MF_INTERNAL_LABEL_CAP];
                mf_block_label(&emit, (anvil_mir_block_t)b, label, sizeof(label));
                anvil_strbuf_appendf(&emit.code, "%-8s DS    0H\n", label);
            }
            for (size_t instr_index = 0;
                 instr_index < anvil_mir_num_instrs(mir) && !emit.code.failed;
                 instr_index++) {
                anvil_mir_instr_info_t instr;
                if (!anvil_mir_get_instr_info(mir, instr_index, &instr)) {
                    ok = false;
                    break;
                }
                if (instr.block != (anvil_mir_block_t)b) continue;
                mf_emit_instr(&emit, instr, instr_index);
            }
            if (!ok || emit.code.failed) break;
        }
        if (!emit.code.failed) {
            mf_emit_string_literals(&emit);
            anvil_strbuf_appendf(&emit.code, "%s_DYN EQU   %d\n",
                                 emit.func_label, emit.frame_size);
        }
    }

    free(emit.frame_offsets);
    free(emit.spill_offsets);
    free(emit.call_arg_value_offsets);
    free(emit.call_arg_is_last);
    free(emit.call_arg_counts);
    if (!ok || emit.code.failed) {
        anvil_strbuf_destroy(&emit.code);
        return false;
    }

    *output = anvil_strbuf_detach(&emit.code, len);
    return *output != NULL;
}

bool anvil_mainframe_emit_mir(const anvil_mir_func_t *mir,
                              anvil_mainframe_variant_t variant,
                              char **output,
                              size_t *len)
{
    const anvil_mainframe_target_desc_t *desc =
        anvil_mainframe_get_target_desc(variant);
    if (!desc) return false;
    return mf_emit_mir_ex(mir, variant, desc->fp_format, output, len);
}

static void mf_emit_data_int(anvil_strbuf_t *out, size_t size, int64_t value)
{
    if (size <= 1) {
        anvil_strbuf_appendf(out, "DC    X'%02llX'\n",
                             (unsigned long long)(value & 0xff));
    } else if (size <= 2) {
        anvil_strbuf_appendf(out, "DC    H'%lld'\n", (long long)value);
    } else if (size <= 4) {
        anvil_strbuf_appendf(out, "DC    F'%lld'\n", (long long)value);
    } else {
        anvil_strbuf_appendf(out, "DC    FD'%lld'\n", (long long)value);
    }
}

static void mf_emit_data_zero(anvil_strbuf_t *out, size_t size)
{
    if (size == 0) return;
    anvil_strbuf_appendf(out, "DC    %zuX'00'\n", size);
}

static void mf_emit_decimal_initializer(anvil_strbuf_t *out,
                                        anvil_type_t *type,
                                        anvil_value_t *init)
{
    const char *digits = anvil_const_decimal_digits(init);
    if (!digits) digits = "0";
    if (anvil_type_decimal_encoding(type) == ANVIL_DECIMAL_PACKED) {
        anvil_strbuf_appendf(out, "DC    PL%zu'%s'\n",
                             anvil_type_size(type), digits);
    } else {
        anvil_strbuf_appendf(out, "DC    ZL%zu'%s'\n",
                             anvil_type_size(type), digits);
    }
}

static void mf_emit_global_initializer(anvil_strbuf_t *out,
                                       anvil_type_t *type,
                                       anvil_value_t *init,
                                       anvil_fp_format_t fp_format)
{
    if (!type) return;
    if (!init) {
        mf_emit_data_zero(out, type->size);
        return;
    }

    switch (type->kind) {
        case ANVIL_TYPE_I1:
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_I64:
            mf_emit_data_int(out, type->size, init->data.i);
            break;
        case ANVIL_TYPE_U8:
        case ANVIL_TYPE_U16:
        case ANVIL_TYPE_U32:
        case ANVIL_TYPE_U64:
        case ANVIL_TYPE_PTR:
            mf_emit_data_int(out, type->size, (int64_t)init->data.u);
            break;
        case ANVIL_TYPE_F32:
            anvil_strbuf_appendf(out, "DC    %s'%g'\n",
                                 fp_format == ANVIL_FP_IEEE754 ||
                                 fp_format == ANVIL_FP_HFP_IEEE ? "EB" : "E",
                                 init->data.f);
            break;
        case ANVIL_TYPE_F64:
            anvil_strbuf_appendf(out, "DC    %s'%g'\n",
                                 fp_format == ANVIL_FP_IEEE754 ||
                                 fp_format == ANVIL_FP_HFP_IEEE ? "DB" : "D",
                                 init->data.f);
            break;
        case ANVIL_TYPE_DECIMAL:
            mf_emit_decimal_initializer(out, type, init);
            break;
        case ANVIL_TYPE_ARRAY:
            if (init->kind == ANVIL_VAL_CONST_ARRAY) {
                for (size_t i = 0; i < init->data.array.num_elements; i++) {
                    mf_emit_global_initializer(out, type->data.array.elem,
                                               init->data.array.elements[i],
                                               fp_format);
                }
            } else {
                mf_emit_data_zero(out, type->size);
            }
            break;
        default:
            mf_emit_data_zero(out, type->size);
            break;
    }
}

static void mf_emit_globals(anvil_strbuf_t *out, anvil_module_t *mod,
                            anvil_fp_format_t fp_format)
{
    if (!mod || !mod->globals) return;
    anvil_strbuf_append(out, "         LTORG\n");
    anvil_strbuf_append(out, "         DS    0D\n");
    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        if (!g->value || !g->value->name) continue;
        char upper[96];
        mf_uppercase(upper, g->value->name, sizeof(upper));
        anvil_strbuf_appendf(out, "%-8s ", upper);
        mf_emit_global_initializer(out, g->value->type,
                                   g->value->data.global.init,
                                   fp_format);
    }
}

anvil_error_t anvil_mainframe_codegen_func(anvil_backend_t *be,
                                           anvil_func_t *func,
                                           anvil_mainframe_variant_t variant,
                                           char **output,
                                           size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    *output = NULL;
    if (len) *len = 0;

    anvil_mir_func_t *mir = anvil_mainframe_lower_func_to_mir(func, variant);
    if (!mir) {
        if (be->ctx) {
            anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN,
                            "mainframe MachineIR lowering failed for function %s",
                            func->name ? func->name : "<anon>");
        }
        return ANVIL_ERR_CODEGEN;
    }

    char legal_error[256] = { 0 };
    bool ok = anvil_mainframe_verify_mir_legal(mir, variant,
                                               legal_error,
                                               sizeof(legal_error));
    if (!ok && be->ctx) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN,
                        "mainframe MachineIR legalization failed for function %s: %s",
                        func->name ? func->name : "<anon>",
                        legal_error[0] ? legal_error : "unknown legalizer error");
    }
    if (ok) ok = anvil_mainframe_regalloc_mir(mir, variant);
    if (!ok && be->ctx && !legal_error[0]) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN,
                        "mainframe MachineIR register allocation failed for function %s",
                        func->name ? func->name : "<anon>");
    }
    if (ok) {
        anvil_fp_format_t fp_format = be->ctx ? be->ctx->fp_format
                                              : anvil_mainframe_get_target_desc(variant)->fp_format;
        ok = mf_emit_mir_ex(mir, variant, fp_format, output, len);
        if (!ok && be->ctx) {
            anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN,
                            "mainframe HLASM emission failed for function %s",
                            func->name ? func->name : "<anon>");
        }
    }
    anvil_mir_func_destroy(mir);
    return ok ? ANVIL_OK : ANVIL_ERR_CODEGEN;
}

anvil_error_t anvil_mainframe_codegen_module(anvil_backend_t *be,
                                             anvil_module_t *mod,
                                             anvil_mainframe_variant_t variant,
                                             char **output,
                                             size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;
    *output = NULL;
    if (len) *len = 0;

    anvil_fp_format_t fp_format = be->ctx ? be->ctx->fp_format
                                          : anvil_mainframe_get_target_desc(variant)->fp_format;
    anvil_strbuf_t out;
    anvil_strbuf_init(&out);

    bool ok = true;
    for (anvil_func_t *func = mod->funcs; ok && func; func = func->next) {
        if (func->is_declaration) continue;
        char *func_text = NULL;
        size_t func_len = 0;
        anvil_error_t err = anvil_mainframe_codegen_func(be, func, variant,
                                                         &func_text, &func_len);
        if (err != ANVIL_OK) {
            ok = false;
            break;
        }
        anvil_strbuf_append(&out, func_text);
        free(func_text);
    }
    if (ok) mf_emit_globals(&out, mod, fp_format);
    if (ok) anvil_strbuf_append(&out, "         END\n");

    if (!ok || out.failed) {
        anvil_strbuf_destroy(&out);
        return ok ? ANVIL_ERR_NOMEM : ANVIL_ERR_CODEGEN;
    }
    *output = anvil_strbuf_detach(&out, len);
    return *output ? ANVIL_OK : ANVIL_ERR_NOMEM;
}
