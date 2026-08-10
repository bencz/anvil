/*
 * ANVIL - Shared PowerPC lowering to MachineIR.
 *
 * PPC32, PPC64 ELFv1, and PPC64LE ELFv2 share source IR lowering,
 * target-aware MIR validation, register allocation, and assembly emission.
 * Variant-specific ABI facts live in target descriptors instead of separate
 * direct emitters.
 */

#include "anvil/anvil_ppc_mir.h"
#include "anvil/anvil_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
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
    anvil_value_t *value;
    int64_t imm;
} ppc_wide_const_t;

typedef struct {
    anvil_value_t *value;
    anvil_mir_vreg_t hi;
    anvil_mir_vreg_t lo;
    bool is_unsigned;
} ppc_wide_pair_t;

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
    const anvil_ppc_target_desc_t *desc;
    anvil_func_t *func;
    anvil_mir_func_t *mir;
    value_vreg_t *values;
    size_t num_values;
    size_t cap_values;
    value_addr_offset_t *addr_offsets;
    size_t num_addr_offsets;
    size_t cap_addr_offsets;
    ppc_wide_const_t *wide_consts;
    size_t num_wide_consts;
    size_t cap_wide_consts;
    ppc_wide_pair_t *wide_pairs;
    size_t num_wide_pairs;
    size_t cap_wide_pairs;
    block_map_t *blocks;
    size_t num_blocks;
    size_t num_edge_blocks;
} ppc_mir_lower_t;

static void ppc_lower_free(ppc_mir_lower_t *lower)
{
    if (!lower) return;
    free(lower->blocks);
    free(lower->values);
    free(lower->addr_offsets);
    free(lower->wide_consts);
    free(lower->wide_pairs);
}

static const int ppc_gpr_arg_regs[] = { 3, 4, 5, 6, 7, 8, 9, 10 };
static const int ppc32_fpr_arg_regs[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static const int ppc64_fpr_arg_regs[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13
};
static const int ppc_alloc_gpr_regs[] = {
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
};
static const int ppc_alloc_fpr_regs[] = {
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30, 31
};
static const int ppc_scratch_gpr_regs[] = { 11, 12 };
static const int ppc_scratch_fpr_regs[] = { 0, 13 };

static const anvil_ppc_target_desc_t ppc_target_descs[] = {
    {
        .variant = ANVIL_PPC_VARIANT_PPC32,
        .arch = ANVIL_ARCH_PPC32,
        .name = "ppc32",
        .word_size = 4,
        .little_endian = false,
        .uses_function_descriptors = false,
        .min_frame_size = 32,
        .lr_save_offset = 4,
        .toc_save_offset = 0,
        .outgoing_arg_offset = 8,
        .incoming_arg_offset = 8,
        .gpr_arg_regs = ppc_gpr_arg_regs,
        .num_gpr_arg_regs = sizeof(ppc_gpr_arg_regs) / sizeof(ppc_gpr_arg_regs[0]),
        .fpr_arg_regs = ppc32_fpr_arg_regs,
        .num_fpr_arg_regs = sizeof(ppc32_fpr_arg_regs) / sizeof(ppc32_fpr_arg_regs[0]),
        .gpr_return_reg = 3,
        .fpr_return_reg = 1,
        .indirect_call_reg = 12,
        .alloc_gpr_regs = ppc_alloc_gpr_regs,
        .num_alloc_gpr_regs = sizeof(ppc_alloc_gpr_regs) / sizeof(ppc_alloc_gpr_regs[0]),
        .alloc_fpr_regs = ppc_alloc_fpr_regs,
        .num_alloc_fpr_regs = sizeof(ppc_alloc_fpr_regs) / sizeof(ppc_alloc_fpr_regs[0]),
        .scratch_gpr_regs = ppc_scratch_gpr_regs,
        .num_scratch_gpr_regs = sizeof(ppc_scratch_gpr_regs) / sizeof(ppc_scratch_gpr_regs[0]),
        .scratch_fpr_regs = ppc_scratch_fpr_regs,
        .num_scratch_fpr_regs = sizeof(ppc_scratch_fpr_regs) / sizeof(ppc_scratch_fpr_regs[0]),
    },
    {
        .variant = ANVIL_PPC_VARIANT_PPC64,
        .arch = ANVIL_ARCH_PPC64,
        .name = "ppc64",
        .word_size = 8,
        .little_endian = false,
        .uses_function_descriptors = true,
        .min_frame_size = 112,
        .lr_save_offset = 16,
        .toc_save_offset = 40,
        .outgoing_arg_offset = 48,
        .incoming_arg_offset = 48,
        .gpr_arg_regs = ppc_gpr_arg_regs,
        .num_gpr_arg_regs = sizeof(ppc_gpr_arg_regs) / sizeof(ppc_gpr_arg_regs[0]),
        .fpr_arg_regs = ppc64_fpr_arg_regs,
        .num_fpr_arg_regs = sizeof(ppc64_fpr_arg_regs) / sizeof(ppc64_fpr_arg_regs[0]),
        .gpr_return_reg = 3,
        .fpr_return_reg = 1,
        .indirect_call_reg = 12,
        .alloc_gpr_regs = ppc_alloc_gpr_regs,
        .num_alloc_gpr_regs = sizeof(ppc_alloc_gpr_regs) / sizeof(ppc_alloc_gpr_regs[0]),
        .alloc_fpr_regs = ppc_alloc_fpr_regs,
        .num_alloc_fpr_regs = sizeof(ppc_alloc_fpr_regs) / sizeof(ppc_alloc_fpr_regs[0]),
        .scratch_gpr_regs = ppc_scratch_gpr_regs,
        .num_scratch_gpr_regs = sizeof(ppc_scratch_gpr_regs) / sizeof(ppc_scratch_gpr_regs[0]),
        .scratch_fpr_regs = ppc_scratch_fpr_regs,
        .num_scratch_fpr_regs = sizeof(ppc_scratch_fpr_regs) / sizeof(ppc_scratch_fpr_regs[0]),
    },
    {
        .variant = ANVIL_PPC_VARIANT_PPC64LE,
        .arch = ANVIL_ARCH_PPC64LE,
        .name = "ppc64le",
        .word_size = 8,
        .little_endian = true,
        .uses_function_descriptors = false,
        .min_frame_size = 32,
        .lr_save_offset = 16,
        .toc_save_offset = 24,
        .outgoing_arg_offset = 32,
        .incoming_arg_offset = 32,
        .gpr_arg_regs = ppc_gpr_arg_regs,
        .num_gpr_arg_regs = sizeof(ppc_gpr_arg_regs) / sizeof(ppc_gpr_arg_regs[0]),
        .fpr_arg_regs = ppc64_fpr_arg_regs,
        .num_fpr_arg_regs = sizeof(ppc64_fpr_arg_regs) / sizeof(ppc64_fpr_arg_regs[0]),
        .gpr_return_reg = 3,
        .fpr_return_reg = 1,
        .indirect_call_reg = 12,
        .alloc_gpr_regs = ppc_alloc_gpr_regs,
        .num_alloc_gpr_regs = sizeof(ppc_alloc_gpr_regs) / sizeof(ppc_alloc_gpr_regs[0]),
        .alloc_fpr_regs = ppc_alloc_fpr_regs,
        .num_alloc_fpr_regs = sizeof(ppc_alloc_fpr_regs) / sizeof(ppc_alloc_fpr_regs[0]),
        .scratch_gpr_regs = ppc_scratch_gpr_regs,
        .num_scratch_gpr_regs = sizeof(ppc_scratch_gpr_regs) / sizeof(ppc_scratch_gpr_regs[0]),
        .scratch_fpr_regs = ppc_scratch_fpr_regs,
        .num_scratch_fpr_regs = sizeof(ppc_scratch_fpr_regs) / sizeof(ppc_scratch_fpr_regs[0]),
    },
};

const anvil_ppc_target_desc_t *
anvil_ppc_get_target_desc(anvil_ppc_variant_t variant)
{
    for (size_t i = 0; i < sizeof(ppc_target_descs) / sizeof(ppc_target_descs[0]); i++) {
        if (ppc_target_descs[i].variant == variant) return &ppc_target_descs[i];
    }
    return NULL;
}

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
    return type && (type->kind == ANVIL_TYPE_I64 ||
                    type->kind == ANVIL_TYPE_U64);
}

static bool ppc_needs_i64_pair(ppc_mir_lower_t *lower, anvil_type_t *type)
{
    return lower && lower->desc && lower->desc->word_size == 4 &&
           ppc_type_is_64bit_integer(type);
}

static bool ppc_ensure_i64_pair(ppc_mir_lower_t *lower,
                                anvil_value_t *value,
                                anvil_mir_vreg_t *hi,
                                anvil_mir_vreg_t *lo,
                                bool *is_unsigned);

static anvil_mir_reg_class_t ppc_reg_class_for_type(anvil_type_t *type)
{
    return ppc_type_is_fp(type) ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR;
}

static uint16_t ppc_bits_for_type(const anvil_ppc_target_desc_t *desc,
                                  anvil_type_t *type)
{
    if (!desc) return 64;
    if (!type || type->kind == ANVIL_TYPE_PTR) return (uint16_t)(desc->word_size * 8);

    size_t size = anvil_type_size(type);
    if (size == 0) return (uint16_t)(desc->word_size * 8);
    if (size > UINT16_MAX / 8) return (uint16_t)(desc->word_size * 8);
    return (uint16_t)(size * 8);
}

static uint16_t ppc_slot_bits_for_type(const anvil_ppc_target_desc_t *desc,
                                       anvil_type_t *type)
{
    size_t size = type ? anvil_type_size(type) : desc->word_size;
    if (size == 0) size = desc->word_size;
    if (size > UINT16_MAX / 8) size = UINT16_MAX / 8;
    return (uint16_t)(size * 8);
}

static uint16_t ppc_align_for_type(const anvil_ppc_target_desc_t *desc,
                                   anvil_type_t *type)
{
    size_t align = type ? anvil_type_align(type) : desc->word_size;
    if (align == 0) align = desc->word_size;
    if (align > desc->word_size) align = desc->word_size;
    if (align > UINT16_MAX) align = UINT16_MAX;
    return (uint16_t)align;
}

static anvil_mir_vreg_t ppc_add_vreg_for_type(ppc_mir_lower_t *lower,
                                              anvil_type_t *type)
{
    return anvil_mir_add_vreg_typed(lower->mir,
                                    ppc_reg_class_for_type(type),
                                    ppc_bits_for_type(lower->desc, type),
                                    ppc_type_is_signed(type));
}

static bool map_reserve(ppc_mir_lower_t *lower, size_t needed)
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

static bool map_put(ppc_mir_lower_t *lower, anvil_value_t *value,
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

static anvil_mir_vreg_t map_get(ppc_mir_lower_t *lower, anvil_value_t *value)
{
    for (size_t i = 0; i < lower->num_values; i++) {
        if (lower->values[i].value == value) return lower->values[i].vreg;
    }
    return ANVIL_MIR_NO_VREG;
}

static bool addr_map_reserve(ppc_mir_lower_t *lower, size_t needed)
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

static bool addr_map_put(ppc_mir_lower_t *lower,
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

static bool addr_map_get(ppc_mir_lower_t *lower,
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

static bool wide_const_reserve(ppc_mir_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_wide_consts) return true;

    size_t new_cap = lower->cap_wide_consts ? lower->cap_wide_consts * 2 : 8;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) return false;
        new_cap *= 2;
    }

    ppc_wide_const_t *grown =
        realloc(lower->wide_consts, new_cap * sizeof(*grown));
    if (!grown) return false;

    lower->wide_consts = grown;
    lower->cap_wide_consts = new_cap;
    return true;
}

static bool wide_const_put(ppc_mir_lower_t *lower,
                           anvil_value_t *value,
                           int64_t imm)
{
    if (!value) return false;

    for (size_t i = 0; i < lower->num_wide_consts; i++) {
        if (lower->wide_consts[i].value == value) {
            lower->wide_consts[i].imm = imm;
            return true;
        }
    }

    if (!wide_const_reserve(lower, lower->num_wide_consts + 1)) return false;
    lower->wide_consts[lower->num_wide_consts].value = value;
    lower->wide_consts[lower->num_wide_consts].imm = imm;
    lower->num_wide_consts++;
    return true;
}

static bool wide_const_get(ppc_mir_lower_t *lower,
                           anvil_value_t *value,
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

static bool wide_pair_reserve(ppc_mir_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_wide_pairs) return true;

    size_t new_cap = lower->cap_wide_pairs ? lower->cap_wide_pairs * 2 : 8;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) return false;
        new_cap *= 2;
    }

    ppc_wide_pair_t *grown =
        realloc(lower->wide_pairs, new_cap * sizeof(*grown));
    if (!grown) return false;

    lower->wide_pairs = grown;
    lower->cap_wide_pairs = new_cap;
    return true;
}

static bool wide_pair_put(ppc_mir_lower_t *lower,
                          anvil_value_t *value,
                          anvil_mir_vreg_t hi,
                          anvil_mir_vreg_t lo,
                          bool is_unsigned)
{
    if (!value || hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG) {
        return false;
    }

    for (size_t i = 0; i < lower->num_wide_pairs; i++) {
        if (lower->wide_pairs[i].value == value) {
            lower->wide_pairs[i].hi = hi;
            lower->wide_pairs[i].lo = lo;
            lower->wide_pairs[i].is_unsigned = is_unsigned;
            return true;
        }
    }

    if (!wide_pair_reserve(lower, lower->num_wide_pairs + 1)) return false;
    lower->wide_pairs[lower->num_wide_pairs].value = value;
    lower->wide_pairs[lower->num_wide_pairs].hi = hi;
    lower->wide_pairs[lower->num_wide_pairs].lo = lo;
    lower->wide_pairs[lower->num_wide_pairs].is_unsigned = is_unsigned;
    lower->num_wide_pairs++;
    return true;
}

static bool wide_pair_get(ppc_mir_lower_t *lower,
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

static anvil_mir_block_t block_get(ppc_mir_lower_t *lower,
                                   anvil_block_t *block)
{
    if (!block) return ANVIL_MIR_NO_BLOCK;
    for (size_t i = 0; i < lower->num_blocks; i++) {
        if (lower->blocks[i].block == block) return lower->blocks[i].mir_block;
    }
    return ANVIL_MIR_NO_BLOCK;
}

static bool create_mir_blocks(ppc_mir_lower_t *lower)
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

static bool set_fixed_register_arg(ppc_mir_lower_t *lower,
                                   anvil_mir_vreg_t vreg,
                                   anvil_type_t *type,
                                   size_t *gpr_count,
                                   size_t *fpr_count)
{
    const anvil_ppc_target_desc_t *desc = lower->desc;
    if (ppc_type_is_fp(type)) {
        if (*fpr_count >= desc->num_fpr_arg_regs) return false;
        if (!anvil_mir_set_fixed_reg(lower->mir, vreg,
                                     desc->fpr_arg_regs[*fpr_count])) {
            return false;
        }
        (*fpr_count)++;
        return true;
    }

    if (*gpr_count >= desc->num_gpr_arg_regs) return false;
    if (!anvil_mir_set_fixed_reg(lower->mir, vreg,
                                 desc->gpr_arg_regs[*gpr_count])) {
        return false;
    }
    (*gpr_count)++;
    return true;
}

static bool arg_still_uses_register(ppc_mir_lower_t *lower,
                                    anvil_type_t *type,
                                    size_t gpr_count,
                                    size_t fpr_count)
{
    const anvil_ppc_target_desc_t *desc = lower->desc;
    return ppc_type_is_fp(type)
        ? fpr_count < desc->num_fpr_arg_regs
        : gpr_count < desc->num_gpr_arg_regs;
}

static void advance_arg_count(anvil_type_t *type,
                              size_t *gpr_count,
                              size_t *fpr_count)
{
    if (ppc_type_is_fp(type)) {
        (*fpr_count)++;
    } else {
        (*gpr_count)++;
    }
}

static int64_t ppc_stack_arg_slot_size(ppc_mir_lower_t *lower,
                                       anvil_type_t *type)
{
    int64_t size = type ? (int64_t)anvil_type_size(type) : (int64_t)lower->desc->word_size;
    int64_t word = (int64_t)lower->desc->word_size;
    if (size <= 0) size = word;
    if (size < word) size = word;
    return (size + word - 1) & ~(word - 1);
}

static bool lower_params(ppc_mir_lower_t *lower)
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

        if (ppc_needs_i64_pair(lower, param->type)) {
            bool is_unsigned = param->type->kind == ANVIL_TYPE_U64;
            anvil_mir_vreg_t hi = anvil_mir_add_vreg_typed(
                lower->mir, ANVIL_MIR_REG_GPR, 32, !is_unsigned);
            anvil_mir_vreg_t lo = anvil_mir_add_vreg_typed(
                lower->mir, ANVIL_MIR_REG_GPR, 32, false);
            if (hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG) {
                return false;
            }

            if (gpr_count + 2 <= lower->desc->num_gpr_arg_regs) {
                anvil_mir_vreg_t incoming_hi = anvil_mir_add_vreg_typed(
                    lower->mir, ANVIL_MIR_REG_GPR, 32, !is_unsigned);
                anvil_mir_vreg_t incoming_lo = anvil_mir_add_vreg_typed(
                    lower->mir, ANVIL_MIR_REG_GPR, 32, false);
                if (incoming_hi == ANVIL_MIR_NO_VREG ||
                    incoming_lo == ANVIL_MIR_NO_VREG ||
                    !anvil_mir_set_fixed_reg(
                        lower->mir, incoming_hi,
                        lower->desc->gpr_arg_regs[gpr_count]) ||
                    !anvil_mir_set_fixed_reg(
                        lower->mir, incoming_lo,
                        lower->desc->gpr_arg_regs[gpr_count + 1]) ||
                    !anvil_mir_set_live_in(lower->mir, incoming_hi, true) ||
                    !anvil_mir_set_live_in(lower->mir, incoming_lo, true)) {
                    return false;
                }
                anvil_mir_vreg_t hi_uses[] = { incoming_hi };
                anvil_mir_vreg_t lo_uses[] = { incoming_lo };
                if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                                         hi, hi_uses, 1) ||
                    !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                                         lo, lo_uses, 1)) {
                    return false;
                }
                gpr_count += 2;
            } else {
                if (!anvil_mir_add_instr_imm(
                        lower->mir, ANVIL_MIR_OP_INCOMING_STACK_ARG,
                        hi, stack_offset) ||
                    !anvil_mir_add_instr_imm(
                        lower->mir, ANVIL_MIR_OP_INCOMING_STACK_ARG,
                        lo, stack_offset + 4)) {
                    return false;
                }
                stack_offset += 8;
                gpr_count += 2;
            }
            if (!wide_pair_put(lower, param, hi, lo, is_unsigned)) {
                return false;
            }
            continue;
        }

        anvil_mir_vreg_t local = ppc_add_vreg_for_type(lower, param->type);
        if (local == ANVIL_MIR_NO_VREG) return false;

        if (arg_still_uses_register(lower, param->type, gpr_count, fpr_count)) {
            anvil_mir_vreg_t incoming = ppc_add_vreg_for_type(lower, param->type);
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
            stack_offset += ppc_stack_arg_slot_size(lower, param->type);
            advance_arg_count(param->type, &gpr_count, &fpr_count);
        }

        if (!map_put(lower, param, local)) return false;
    }

    return true;
}

static bool prepare_phi_results(ppc_mir_lower_t *lower)
{
    for (anvil_block_t *block = lower->func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_PHI) break;
            if (!instr->result) return false;

            anvil_mir_vreg_t vreg = ppc_add_vreg_for_type(lower,
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

static int64_t ppc_int_constant(anvil_value_t *value)
{
    if (!value) return 0;
    if (value->type && !value->type->is_signed) return (int64_t)value->data.u;
    return value->data.i;
}

static bool ppc_get_const_int(anvil_value_t *value, int64_t *out)
{
    if (!value || value->kind != ANVIL_VAL_CONST_INT) return false;
    if (out) *out = ppc_int_constant(value);
    return true;
}

static anvil_mir_vreg_t lower_value(ppc_mir_lower_t *lower,
                                    anvil_value_t *value);
static bool lower_add_const_offset(ppc_mir_lower_t *lower,
                                   anvil_mir_vreg_t base,
                                   int64_t offset,
                                   anvil_mir_vreg_t *out_ptr);
static bool ppc_lower_i64_bitcast_pair(ppc_mir_lower_t *lower,
                                       anvil_instr_t *instr);

static anvil_mir_vreg_t lower_const_value(ppc_mir_lower_t *lower,
                                          anvil_value_t *value)
{
    anvil_mir_vreg_t vreg = ppc_add_vreg_for_type(lower, value->type);
    if (vreg == ANVIL_MIR_NO_VREG) return ANVIL_MIR_NO_VREG;

    int64_t imm = 0;
    switch (value->kind) {
        case ANVIL_VAL_CONST_INT:
            imm = ppc_int_constant(value);
            break;
        case ANVIL_VAL_CONST_NULL:
            imm = 0;
            break;
        case ANVIL_VAL_CONST_FLOAT:
            imm = float_bits_as_i64(value->data.f,
                                    ppc_bits_for_type(lower->desc, value->type));
            break;
        default:
            return ANVIL_MIR_NO_VREG;
    }

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, vreg, imm)) {
        return ANVIL_MIR_NO_VREG;
    }
    return vreg;
}

static anvil_mir_vreg_t lower_symbol_address(ppc_mir_lower_t *lower,
                                             const char *symbol)
{
    if (!symbol || !symbol[0]) return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t vreg =
        anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR,
                                 (uint16_t)(lower->desc->word_size * 8),
                                 false);
    if (vreg == ANVIL_MIR_NO_VREG) return ANVIL_MIR_NO_VREG;

    if (!anvil_mir_add_instr_symbol(lower->mir, ANVIL_MIR_OP_SYMBOL_ADDR,
                                    vreg, NULL, 0, symbol)) {
        return ANVIL_MIR_NO_VREG;
    }
    return vreg;
}

static anvil_mir_vreg_t lower_string_address(ppc_mir_lower_t *lower,
                                             anvil_value_t *value)
{
    const char *label = NULL;
    if (anvil_mir_add_string_literal(lower->mir, value->data.str,
                                     &label) < 0 || !label) {
        return ANVIL_MIR_NO_VREG;
    }
    return lower_symbol_address(lower, label);
}

static anvil_mir_vreg_t lower_value(ppc_mir_lower_t *lower,
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

static bool add_return_copy(ppc_mir_lower_t *lower, anvil_value_t *value)
{
    if (!value) {
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET,
                                   ANVIL_MIR_NO_VREG, NULL, 0);
    }


    if (ppc_needs_i64_pair(lower, value->type)) {
        anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
        anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
        if (!wide_pair_get(lower, value, &hi, &lo, NULL)) return false;
        const anvil_mir_vreg_info_t *hi_info =
            anvil_mir_get_vreg_info(lower->mir, hi);
        if (!hi_info) return false;
        anvil_mir_vreg_t ret_hi = anvil_mir_add_vreg_typed(
            lower->mir, ANVIL_MIR_REG_GPR, 32, hi_info->is_signed);
        anvil_mir_vreg_t ret_lo = anvil_mir_add_vreg_typed(
            lower->mir, ANVIL_MIR_REG_GPR, 32, false);
        if (ret_hi == ANVIL_MIR_NO_VREG || ret_lo == ANVIL_MIR_NO_VREG ||
            !anvil_mir_set_fixed_reg(lower->mir, ret_hi, 3) ||
            !anvil_mir_set_fixed_reg(lower->mir, ret_lo, 4)) {
            return false;
        }
        anvil_mir_vreg_t hi_uses[] = { hi };
        anvil_mir_vreg_t lo_uses[] = { lo };
        anvil_mir_vreg_t ret_uses[] = { ret_hi };
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                                   ret_hi, hi_uses, 1) &&
               anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                                   ret_lo, lo_uses, 1) &&
               anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET,
                                   ANVIL_MIR_NO_VREG, ret_uses, 1);
    }

    anvil_mir_vreg_t src = lower_value(lower, value);
    if (src == ANVIL_MIR_NO_VREG) return false;

    anvil_mir_vreg_t ret = ppc_add_vreg_for_type(lower, value->type);
    if (ret == ANVIL_MIR_NO_VREG) return false;
    if (!anvil_mir_set_fixed_reg(
            lower->mir,
            ret,
            ppc_type_is_fp(value->type)
                ? lower->desc->fpr_return_reg
                : lower->desc->gpr_return_reg)) {
        return false;
    }

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

static bool break_parallel_copy_cycle(ppc_mir_lower_t *lower,
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
        if (copies[i].src == saved) copies[i].src = temp;
    }
    return true;
}

static bool emit_parallel_phi_edge_copies(ppc_mir_lower_t *lower,
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
        if (!break_parallel_copy_cycle(lower, copies, num_copies)) return false;
    }

    return true;
}

static bool lower_phi_copies_for_edge(ppc_mir_lower_t *lower,
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

        if (!found) goto fail;
    }

    bool ok = emit_parallel_phi_edge_copies(lower, copies, num_copies);
    free(copies);
    return ok;

fail:
    free(copies);
    return false;
}

static anvil_mir_block_t create_phi_edge_block(ppc_mir_lower_t *lower,
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

static bool emit_phi_edge_block(ppc_mir_lower_t *lower,
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

static bool prepare_phi_aware_target(ppc_mir_lower_t *lower,
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

static bool emit_pending_phi_edges(ppc_mir_lower_t *lower,
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

static anvil_mir_block_t create_switch_chain_block(ppc_mir_lower_t *lower,
                                                   anvil_block_t *src_block,
                                                   size_t case_index)
{
    const char *src_name = (src_block && src_block->name) ? src_block->name : "anon";

    char name[256];
    snprintf(name, sizeof(name), "%s_switch_case_%zu_%zu",
             src_name, case_index, lower->num_edge_blocks++);
    return anvil_mir_add_block(lower->mir, name);
}

static bool lower_call(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands == 0) return false;

    anvil_value_t *callee = instr->operands[0];
    anvil_type_t *fn_type = call_func_type(callee);
    if (!fn_type) return false;

    bool direct_call = call_is_direct_symbol(callee);
    const char *symbol = direct_call ? call_symbol(callee) : NULL;
    if (direct_call && !symbol) return false;

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
        anvil_mir_vreg_t target_fixed =
            anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR,
                                     (uint16_t)(lower->desc->word_size * 8),
                                     false);
        if (target_src == ANVIL_MIR_NO_VREG ||
            target_fixed == ANVIL_MIR_NO_VREG ||
            !anvil_mir_set_fixed_reg(lower->mir, target_fixed,
                                     lower->desc->indirect_call_reg)) {
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

        if (!arg_still_uses_register(lower, arg->type, gpr_count, fpr_count)) {
            anvil_mir_vreg_t stack_use[] = { src };
            if (!anvil_mir_add_instr_imm_uses(lower->mir,
                                              ANVIL_MIR_OP_CALL_STACK_ARG,
                                              ANVIL_MIR_NO_VREG,
                                              stack_use, 1,
                                              stack_offset)) {
                ok = false;
                break;
            }
            stack_offset += ppc_stack_arg_slot_size(lower, arg->type);
            advance_arg_count(arg->type, &gpr_count, &fpr_count);
            continue;
        }

        anvil_mir_vreg_t fixed_arg = ppc_add_vreg_for_type(lower, arg->type);
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
    if (instr->result && !ppc_type_is_void(instr->result->type)) {
        call_def = ppc_add_vreg_for_type(lower, instr->result->type);
        if (call_def == ANVIL_MIR_NO_VREG ||
            !anvil_mir_set_fixed_reg(
                lower->mir,
                call_def,
                ppc_type_is_fp(instr->result->type)
                    ? lower->desc->fpr_return_reg
                    : lower->desc->gpr_return_reg)) {
            free(call_uses);
            return false;
        }
    }

    ok = anvil_mir_add_instr_symbol(lower->mir, ANVIL_MIR_OP_CALL, call_def,
                                    call_uses, num_call_uses, symbol);
    free(call_uses);
    if (!ok) return false;

    if (instr->result && call_def != ANVIL_MIR_NO_VREG) {
        anvil_mir_vreg_t local_result =
            ppc_add_vreg_for_type(lower, instr->result->type);
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

static anvil_mir_vreg_t lower_widen_gpr_to_word(ppc_mir_lower_t *lower,
                                                anvil_mir_vreg_t src,
                                                bool sign_extend)
{
    const anvil_mir_vreg_info_t *src_info =
        anvil_mir_get_vreg_info(lower->mir, src);
    if (!src_info || src_info->reg_class != ANVIL_MIR_REG_GPR) {
        return ANVIL_MIR_NO_VREG;
    }

    uint16_t word_bits = (uint16_t)(lower->desc->word_size * 8);
    if (src_info->size_bits == word_bits) return src;

    if (src_info->size_bits > word_bits) {
        anvil_mir_vreg_t narrow = anvil_mir_add_vreg_typed(
            lower->mir, ANVIL_MIR_REG_GPR, word_bits, sign_extend);
        if (narrow == ANVIL_MIR_NO_VREG) return ANVIL_MIR_NO_VREG;
        anvil_mir_vreg_t uses[] = { src };
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_TRUNC,
                                 narrow, uses, 1)) {
            return ANVIL_MIR_NO_VREG;
        }
        return narrow;
    }

    anvil_mir_vreg_t wide =
        anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR,
                                 word_bits, sign_extend);
    if (wide == ANVIL_MIR_NO_VREG) return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t uses[] = { src };
    anvil_mir_opcode_t op = sign_extend ? ANVIL_MIR_OP_SEXT
                                        : ANVIL_MIR_OP_ZEXT;
    if (!anvil_mir_add_instr(lower->mir, op, wide, uses, 1)) {
        return ANVIL_MIR_NO_VREG;
    }
    return wide;
}

static anvil_mir_vreg_t lower_resize_gpr(ppc_mir_lower_t *lower,
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

static bool lower_match_binary_operand_sizes(ppc_mir_lower_t *lower,
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

static bool lower_alloca(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || !instr->result->type ||
        instr->result->type->kind != ANVIL_TYPE_PTR) {
        return false;
    }

    anvil_type_t *element_type = instr->aux_type;
    if (!element_type) element_type = instr->result->type->data.pointee;
    if (!element_type) return false;

    anvil_mir_vreg_t ptr = ppc_add_vreg_for_type(lower, instr->result->type);
    if (ptr == ANVIL_MIR_NO_VREG) return false;

    if (instr->num_operands == 0) {
        int slot = anvil_mir_add_frame_slot(
            lower->mir,
            ppc_slot_bits_for_type(lower->desc, element_type),
            ppc_align_for_type(lower->desc, element_type));
        if (slot < 0) return false;
        if (!anvil_mir_add_frame_addr(lower->mir, ptr, slot)) return false;
        return map_put(lower, instr->result, ptr);
    }

    if (instr->num_operands != 1) return false;
    anvil_mir_vreg_t count = lower_value(lower, instr->operands[0]);
    if (count == ANVIL_MIR_NO_VREG) return false;
    count = lower_widen_gpr_to_word(lower, count, false);
    if (count == ANVIL_MIR_NO_VREG) return false;

    int64_t elem_size = element_type ? (int64_t)anvil_type_size(element_type) : 1;
    if (elem_size <= 0) elem_size = 1;
    anvil_mir_vreg_t uses[] = { count };
    if (!anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_DYN_ALLOCA,
                                      ptr, uses, 1, elem_size)) {
        return false;
    }
    return map_put(lower, instr->result, ptr);
}

static bool lower_add_const_offset(ppc_mir_lower_t *lower,
                                   anvil_mir_vreg_t base,
                                   int64_t offset,
                                   anvil_mir_vreg_t *out_ptr)
{
    if (offset == 0) {
        *out_ptr = base;
        return true;
    }

    anvil_mir_vreg_t off =
        anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR,
                              (uint16_t)(lower->desc->word_size * 8));
    anvil_mir_vreg_t dst =
        anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR,
                              (uint16_t)(lower->desc->word_size * 8));
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

static bool lower_gep(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands < 2 || !instr->result || !instr->aux_type)
        return false;

    anvil_mir_vreg_t current = lower_value(lower, instr->operands[0]);
    if (current == ANVIL_MIR_NO_VREG) return false;
    current = lower_widen_gpr_to_word(lower, current, false);
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
        anvil_mir_vreg_t index = ANVIL_MIR_NO_VREG;
        if (ppc_needs_i64_pair(lower, index_value->type)) {
            anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
            if (!ppc_ensure_i64_pair(lower, index_value, &hi, &index, NULL))
                return false;
            /* PPC32 represents i64 values as {hi,lo}; the low word is the
             * required pointer-width truncation. */
        } else {
            index = lower_value(lower, index_value);
            if (index == ANVIL_MIR_NO_VREG) return false;
            index = lower_widen_gpr_to_word(lower, index,
                                            index_value->type->is_signed);
        }
        if (index == ANVIL_MIR_NO_VREG) return false;

        anvil_mir_vreg_t scaled = index;
        if (elem_size != 1) {
            anvil_mir_vreg_t scale =
                anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR,
                                      (uint16_t)(lower->desc->word_size * 8));
            scaled = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR,
                                           (uint16_t)(lower->desc->word_size * 8));
            if (scale == ANVIL_MIR_NO_VREG || scaled == ANVIL_MIR_NO_VREG) {
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
            anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR,
                                  (uint16_t)(lower->desc->word_size * 8));
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

static bool lower_struct_gep(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands < 1 || !instr->result) return false;

    anvil_mir_vreg_t base = lower_value(lower, instr->operands[0]);
    if (base == ANVIL_MIR_NO_VREG) return false;
    base = lower_widen_gpr_to_word(lower, base, false);
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

static bool ppc_ensure_i64_pair(ppc_mir_lower_t *lower,
                                anvil_value_t *value,
                                anvil_mir_vreg_t *hi,
                                anvil_mir_vreg_t *lo,
                                bool *is_unsigned);

static bool ppc_lower_i64_pair_to_fp(ppc_mir_lower_t *lower,
                                     anvil_instr_t *instr,
                                     anvil_mir_opcode_t mir_op)
{
    anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
    if (!ppc_ensure_i64_pair(lower, instr->operands[0], &hi, &lo, NULL)) {
        return false;
    }

    bool is_unsigned = mir_op == ANVIL_MIR_OP_UITOFP;
    uint16_t dst_bits = ppc_bits_for_type(lower->desc, instr->result->type);
    anvil_mir_vreg_t arg_hi = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_GPR, 32, !is_unsigned);
    anvil_mir_vreg_t arg_lo = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_GPR, 32, false);
    anvil_mir_vreg_t call_result = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_FPR, dst_bits, false);
    anvil_mir_vreg_t local_result = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_FPR, dst_bits, false);
    if (arg_hi == ANVIL_MIR_NO_VREG || arg_lo == ANVIL_MIR_NO_VREG ||
        call_result == ANVIL_MIR_NO_VREG ||
        local_result == ANVIL_MIR_NO_VREG ||
        !anvil_mir_set_fixed_reg(lower->mir, arg_hi, 3) ||
        !anvil_mir_set_fixed_reg(lower->mir, arg_lo, 4) ||
        !anvil_mir_set_fixed_reg(lower->mir, call_result, 1)) {
        return false;
    }
    anvil_mir_vreg_t hi_uses[] = { hi };
    anvil_mir_vreg_t lo_uses[] = { lo };
    anvil_mir_vreg_t call_uses[] = { arg_hi, arg_lo };
    anvil_mir_vreg_t result_uses[] = { call_result };
    const char *helper = is_unsigned
        ? (dst_bits == 32 ? "__floatundisf" : "__floatundidf")
        : (dst_bits == 32 ? "__floatdisf" : "__floatdidf");
    return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                               arg_hi, hi_uses, 1) &&
           anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                               arg_lo, lo_uses, 1) &&
           anvil_mir_add_instr_symbol(lower->mir, ANVIL_MIR_OP_CALL,
                                      call_result, call_uses, 2, helper) &&
           anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                               local_result, result_uses, 1) &&
           map_put(lower, instr->result, local_result);
}

static bool ppc_lower_fp_to_i64_pair(ppc_mir_lower_t *lower,
                                     anvil_instr_t *instr,
                                     anvil_mir_opcode_t mir_op)
{
    anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
    if (src == ANVIL_MIR_NO_VREG) return false;
    bool is_unsigned = mir_op == ANVIL_MIR_OP_FPTOUI;
    const anvil_mir_vreg_info_t *src_info =
        anvil_mir_get_vreg_info(lower->mir, src);
    if (!src_info || src_info->reg_class != ANVIL_MIR_REG_FPR) return false;
    anvil_mir_vreg_t arg = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_FPR, src_info->size_bits, false);
    anvil_mir_vreg_t ret_hi = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_GPR, 32, !is_unsigned);
    anvil_mir_vreg_t ret_lo = anvil_mir_add_vreg_typed(
        lower->mir, ANVIL_MIR_REG_GPR, 32, false);
    if (arg == ANVIL_MIR_NO_VREG || ret_hi == ANVIL_MIR_NO_VREG ||
        ret_lo == ANVIL_MIR_NO_VREG ||
        !anvil_mir_set_fixed_reg(lower->mir, arg, 1) ||
        !anvil_mir_set_fixed_reg(lower->mir, ret_hi, 3) ||
        !anvil_mir_set_fixed_reg(lower->mir, ret_lo, 4)) {
        return false;
    }
    anvil_mir_vreg_t arg_copy_uses[] = { src };
    anvil_mir_vreg_t call_uses[] = { arg };
    const char *helper;
    if (is_unsigned) {
        helper = src_info->size_bits == 32 ? "__fixunssfdi" : "__fixunsdfdi";
    } else {
        helper = src_info->size_bits == 32 ? "__fixsfdi" : "__fixdfdi";
    }
    return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY,
                               arg, arg_copy_uses, 1) &&
           anvil_mir_add_instr_symbol(lower->mir, ANVIL_MIR_OP_CALL,
                                      ret_hi, call_uses, 1, helper) &&
           anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_CALL_RESULT,
                               ret_lo, NULL, 0) &&
           wide_pair_put(lower, instr->result, ret_hi, ret_lo, is_unsigned);
}

static bool lower_cast(ppc_mir_lower_t *lower,
                       anvil_instr_t *instr,
                       anvil_mir_opcode_t mir_op)
{
    if (instr->num_operands != 1 || !instr->result) return false;
    if (ppc_needs_i64_pair(lower, instr->operands[0]->type) &&
        (mir_op == ANVIL_MIR_OP_SITOFP ||
         mir_op == ANVIL_MIR_OP_UITOFP)) {
        return ppc_lower_i64_pair_to_fp(lower, instr, mir_op);
    }
    if (ppc_needs_i64_pair(lower, instr->result->type)) {
        if (mir_op == ANVIL_MIR_OP_FPTOSI ||
            mir_op == ANVIL_MIR_OP_FPTOUI) {
            return ppc_lower_fp_to_i64_pair(lower, instr, mir_op);
        }
        if (mir_op == ANVIL_MIR_OP_BITCAST) {
            return ppc_lower_i64_bitcast_pair(lower, instr);
        }
        return false;
    }

    anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t def = ppc_add_vreg_for_type(lower, instr->result->type);
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
                instr->op != ANVIL_OP_INTTOPTR) return false;
            mir_op = src_info->size_bits < dst_info->size_bits
                ? ANVIL_MIR_OP_ZEXT : ANVIL_MIR_OP_TRUNC;
        }
    }

    anvil_mir_vreg_t uses[] = { src };
    if (!anvil_mir_add_instr(lower->mir, mir_op, def, uses, 1)) return false;
    return map_put(lower, instr->result, def);
}

static bool lower_select(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands != 3 || !instr->result) return false;
    anvil_mir_vreg_t cond = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t then_v = lower_value(lower, instr->operands[1]);
    anvil_mir_vreg_t else_v = lower_value(lower, instr->operands[2]);
    anvil_mir_vreg_t def = ppc_add_vreg_for_type(lower, instr->result->type);
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

static bool lower_memory_address(ppc_mir_lower_t *lower,
                                 anvil_value_t *value,
                                 anvil_mir_vreg_t *out_base,
                                 int64_t *out_offset)
{
    anvil_mir_vreg_t base = ANVIL_MIR_NO_VREG;
    int64_t offset = 0;
    if (addr_map_get(lower, value, &base, &offset)) {
        base = lower_widen_gpr_to_word(lower, base, false);
        if (base == ANVIL_MIR_NO_VREG) return false;
        *out_base = base;
        *out_offset = offset;
        return true;
    }

    base = lower_value(lower, value);
    if (base == ANVIL_MIR_NO_VREG) return false;
    base = lower_widen_gpr_to_word(lower, base, false);
    if (base == ANVIL_MIR_NO_VREG) return false;

    *out_base = base;
    *out_offset = 0;
    return true;
}

static bool ppc_add_memory_instr(ppc_mir_lower_t *lower,
                                 anvil_mir_opcode_t op,
                                 anvil_mir_vreg_t def,
                                 anvil_mir_vreg_t *uses,
                                 size_t num_uses,
                                 int64_t offset)
{
    if (offset == 0) {
        return anvil_mir_add_instr(lower->mir, op, def, uses, num_uses);
    }
    return anvil_mir_add_instr_imm_uses(lower->mir, op, def, uses, num_uses,
                                        offset);
}

static bool ppc_get_wide_const_value(ppc_mir_lower_t *lower,
                                     anvil_value_t *value,
                                     int64_t *out)
{
    if (ppc_get_const_int(value, out)) return true;
    return wide_const_get(lower, value, out);
}

static anvil_mir_vreg_t ppc_add_i32_vreg(ppc_mir_lower_t *lower,
                                         bool is_signed)
{
    return anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32,
                                    is_signed);
}

static anvil_mir_vreg_t ppc_add_bool_vreg(ppc_mir_lower_t *lower)
{
    return anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
}

static bool ppc_lower_i64_const_pair(ppc_mir_lower_t *lower,
                                     anvil_value_t *value)
{
    anvil_mir_vreg_t existing_hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t existing_lo = ANVIL_MIR_NO_VREG;
    if (wide_pair_get(lower, value, &existing_hi, &existing_lo, NULL)) {
        return true;
    }

    int64_t imm = 0;
    if (!ppc_get_const_int(value, &imm)) return false;

    uint64_t bits = (uint64_t)imm;
    int32_t hi_imm = (int32_t)(bits >> 32);
    int32_t lo_imm = (int32_t)(bits & 0xffffffffu);
    bool is_unsigned = value->type && value->type->kind == ANVIL_TYPE_U64;

    anvil_mir_vreg_t hi = ppc_add_i32_vreg(lower, !is_unsigned);
    anvil_mir_vreg_t lo = ppc_add_i32_vreg(lower, false);
    if (hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG) return false;

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV,
                                 hi, (int64_t)hi_imm) ||
        !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV,
                                 lo, (int64_t)lo_imm)) {
        return false;
    }

    return wide_const_put(lower, value, imm) &&
           wide_pair_put(lower, value, hi, lo, is_unsigned);
}

static bool ppc_ensure_i64_pair(ppc_mir_lower_t *lower,
                                anvil_value_t *value,
                                anvil_mir_vreg_t *hi,
                                anvil_mir_vreg_t *lo,
                                bool *is_unsigned)
{
    if (wide_pair_get(lower, value, hi, lo, is_unsigned)) return true;
    if (value && value->kind == ANVIL_VAL_CONST_INT &&
        ppc_lower_i64_const_pair(lower, value)) {
        return wide_pair_get(lower, value, hi, lo, is_unsigned);
    }
    return false;
}

static bool ppc_lower_load_i64_pair(ppc_mir_lower_t *lower,
                                    anvil_instr_t *instr)
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
    anvil_mir_vreg_t hi = ppc_add_i32_vreg(lower, !is_unsigned);
    anvil_mir_vreg_t lo = ppc_add_i32_vreg(lower, false);
    if (hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG) return false;

    anvil_mir_vreg_t hi_uses[] = { base };
    if (!ppc_add_memory_instr(lower, ANVIL_MIR_OP_LOAD,
                              hi, hi_uses, 1, offset)) {
        return false;
    }

    anvil_mir_vreg_t lo_uses[] = { base };
    if (!ppc_add_memory_instr(lower, ANVIL_MIR_OP_LOAD,
                              lo, lo_uses, 1, offset + 4)) {
        return false;
    }

    return wide_pair_put(lower, instr->result, hi, lo, is_unsigned);
}

static bool ppc_lower_store_i64_const(ppc_mir_lower_t *lower,
                                      int64_t imm,
                                      anvil_mir_vreg_t base,
                                      int64_t offset)
{
    if (offset > INT64_MAX - 4) return false;

    uint64_t bits = (uint64_t)imm;
    int32_t hi_imm = (int32_t)(bits >> 32);
    int32_t lo_imm = (int32_t)(bits & 0xffffffffu);

    anvil_mir_vreg_t hi = ppc_add_i32_vreg(lower, true);
    anvil_mir_vreg_t lo = ppc_add_i32_vreg(lower, false);
    if (hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG) return false;

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV,
                                 hi, (int64_t)hi_imm)) {
        return false;
    }
    anvil_mir_vreg_t hi_uses[] = { hi, base };
    if (!ppc_add_memory_instr(lower, ANVIL_MIR_OP_STORE,
                              ANVIL_MIR_NO_VREG, hi_uses, 2, offset)) {
        return false;
    }

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV,
                                 lo, (int64_t)lo_imm)) {
        return false;
    }
    anvil_mir_vreg_t lo_uses[] = { lo, base };
    return ppc_add_memory_instr(lower, ANVIL_MIR_OP_STORE,
                                ANVIL_MIR_NO_VREG, lo_uses, 2, offset + 4);
}

static bool ppc_lower_store_i64_pair(ppc_mir_lower_t *lower,
                                     anvil_value_t *value,
                                     anvil_mir_vreg_t base,
                                     int64_t offset)
{
    if (offset > INT64_MAX - 4) return false;

    anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
    if (!ppc_ensure_i64_pair(lower, value, &hi, &lo, NULL)) return false;

    anvil_mir_vreg_t hi_uses[] = { hi, base };
    if (!ppc_add_memory_instr(lower, ANVIL_MIR_OP_STORE,
                              ANVIL_MIR_NO_VREG, hi_uses, 2, offset)) {
        return false;
    }

    anvil_mir_vreg_t lo_uses[] = { lo, base };
    return ppc_add_memory_instr(lower, ANVIL_MIR_OP_STORE,
                                ANVIL_MIR_NO_VREG, lo_uses, 2, offset + 4);
}

static bool ppc_lower_i64_bitcast_pair(ppc_mir_lower_t *lower,
                                       anvil_instr_t *instr)
{
    if (!instr->result || instr->num_operands != 1) return false;

    anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
    if (!ppc_ensure_i64_pair(lower, instr->operands[0], &hi, &lo, NULL)) {
        return false;
    }

    int64_t imm = 0;
    if (ppc_get_wide_const_value(lower, instr->operands[0], &imm) &&
        !wide_const_put(lower, instr->result, imm)) {
        return false;
    }

    bool is_unsigned = instr->result->type &&
                       instr->result->type->kind == ANVIL_TYPE_U64;
    return wide_pair_put(lower, instr->result, hi, lo, is_unsigned);
}

static bool ppc_add_pair_cmp(ppc_mir_lower_t *lower,
                             anvil_mir_opcode_t op,
                             anvil_mir_vreg_t def,
                             anvil_mir_vreg_t lhs,
                             anvil_mir_vreg_t rhs)
{
    anvil_mir_vreg_t uses[] = { lhs, rhs };
    return anvil_mir_add_instr(lower->mir, op, def, uses, 2);
}

static bool ppc_lower_i64_cmp_pair(ppc_mir_lower_t *lower,
                                   anvil_instr_t *instr,
                                   anvil_mir_opcode_t op)
{
    if (!instr->result || instr->num_operands != 2) return false;

    anvil_mir_vreg_t lhi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t llo = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t rhi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t rlo = ANVIL_MIR_NO_VREG;
    if (!ppc_ensure_i64_pair(lower, instr->operands[0], &lhi, &llo, NULL) ||
        !ppc_ensure_i64_pair(lower, instr->operands[1], &rhi, &rlo, NULL)) {
        return false;
    }

    anvil_mir_vreg_t result = ppc_add_bool_vreg(lower);
    if (result == ANVIL_MIR_NO_VREG) return false;

    if (op == ANVIL_MIR_OP_CMP_EQ || op == ANVIL_MIR_OP_CMP_NE) {
        anvil_mir_vreg_t hi_cmp = ppc_add_bool_vreg(lower);
        anvil_mir_vreg_t lo_cmp = ppc_add_bool_vreg(lower);
        if (hi_cmp == ANVIL_MIR_NO_VREG ||
            lo_cmp == ANVIL_MIR_NO_VREG) {
            return false;
        }
        if (!ppc_add_pair_cmp(lower, op, hi_cmp, lhi, rhi) ||
            !ppc_add_pair_cmp(lower, op, lo_cmp, llo, rlo)) {
            return false;
        }

        anvil_mir_vreg_t uses[] = { hi_cmp, lo_cmp };
        anvil_mir_opcode_t join = op == ANVIL_MIR_OP_CMP_EQ
                                      ? ANVIL_MIR_OP_AND
                                      : ANVIL_MIR_OP_OR;
        if (!anvil_mir_add_instr(lower->mir, join, result, uses, 2)) {
            return false;
        }
        return map_put(lower, instr->result, result);
    }

    bool unsigned_cmp = op == ANVIL_MIR_OP_CMP_ULT ||
                        op == ANVIL_MIR_OP_CMP_ULE ||
                        op == ANVIL_MIR_OP_CMP_UGT ||
                        op == ANVIL_MIR_OP_CMP_UGE;
    bool less = op == ANVIL_MIR_OP_CMP_LT ||
                op == ANVIL_MIR_OP_CMP_LE ||
                op == ANVIL_MIR_OP_CMP_ULT ||
                op == ANVIL_MIR_OP_CMP_ULE;
    bool equal_ok = op == ANVIL_MIR_OP_CMP_LE ||
                    op == ANVIL_MIR_OP_CMP_GE ||
                    op == ANVIL_MIR_OP_CMP_ULE ||
                    op == ANVIL_MIR_OP_CMP_UGE;

    anvil_mir_opcode_t hi_rel;
    anvil_mir_opcode_t lo_rel;
    if (less) {
        hi_rel = unsigned_cmp ? ANVIL_MIR_OP_CMP_ULT : ANVIL_MIR_OP_CMP_LT;
        lo_rel = equal_ok ? ANVIL_MIR_OP_CMP_ULE : ANVIL_MIR_OP_CMP_ULT;
    } else {
        hi_rel = unsigned_cmp ? ANVIL_MIR_OP_CMP_UGT : ANVIL_MIR_OP_CMP_GT;
        lo_rel = equal_ok ? ANVIL_MIR_OP_CMP_UGE : ANVIL_MIR_OP_CMP_UGT;
    }

    anvil_mir_vreg_t hi_cmp = ppc_add_bool_vreg(lower);
    anvil_mir_vreg_t hi_eq = ppc_add_bool_vreg(lower);
    anvil_mir_vreg_t lo_cmp = ppc_add_bool_vreg(lower);
    anvil_mir_vreg_t eq_and_lo = ppc_add_bool_vreg(lower);
    if (hi_cmp == ANVIL_MIR_NO_VREG ||
        hi_eq == ANVIL_MIR_NO_VREG ||
        lo_cmp == ANVIL_MIR_NO_VREG ||
        eq_and_lo == ANVIL_MIR_NO_VREG) {
        return false;
    }

    if (!ppc_add_pair_cmp(lower, hi_rel, hi_cmp, lhi, rhi) ||
        !ppc_add_pair_cmp(lower, ANVIL_MIR_OP_CMP_EQ, hi_eq, lhi, rhi) ||
        !ppc_add_pair_cmp(lower, lo_rel, lo_cmp, llo, rlo)) {
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

static bool lower_switch(ppc_mir_lower_t *lower, anvil_instr_t *instr)
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

    if (ok) ok = emit_pending_phi_edges(lower, instr->parent, edges, num_edges);
    free(edges);
    return ok;
}

static bool lower_instr(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->op == ANVIL_OP_NOP) return true;
    if (instr->op == ANVIL_OP_PHI) return true;

    anvil_mir_opcode_t mir_op;
    if (instr->num_operands == 2 && map_binop(instr->op, &mir_op)) {
        if (ppc_needs_i64_pair(lower, instr->operands[0]->type) &&
            ppc_needs_i64_pair(lower, instr->operands[1]->type) &&
            mir_op_is_compare(mir_op)) {
            return ppc_lower_i64_cmp_pair(lower, instr, mir_op);
        }

        anvil_mir_vreg_t lhs = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t rhs = lower_value(lower, instr->operands[1]);
        anvil_mir_vreg_t def = instr->result
            ? ppc_add_vreg_for_type(lower, instr->result->type)
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
        if (instr->result &&
            ppc_needs_i64_pair(lower, instr->result->type)) {
            int64_t imm = 0;
            if (!ppc_get_wide_const_value(lower, instr->operands[0], &imm)) {
                return false;
            }
            if (instr->op == ANVIL_OP_NEG) {
                return wide_const_put(lower, instr->result,
                                      (int64_t)(0 - (uint64_t)imm));
            }
            if (instr->op == ANVIL_OP_NOT) {
                return wide_const_put(lower, instr->result, ~imm);
            }
            return false;
        }

        anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t def = instr->result
            ? ppc_add_vreg_for_type(lower, instr->result->type)
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
            if (ppc_needs_i64_pair(lower, instr->result->type)) {
                return ppc_lower_load_i64_pair(lower, instr);
            }

            anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
            int64_t offset = 0;
            if (!lower_memory_address(lower, instr->operands[0],
                                      &ptr, &offset)) {
                return false;
            }
            anvil_mir_vreg_t def = ppc_add_vreg_for_type(lower,
                                                         instr->result->type);
            if (def == ANVIL_MIR_NO_VREG) return false;
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
            if (!ok) return false;
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
            if (ppc_needs_i64_pair(lower, instr->operands[0]->type)) {
                anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
                int64_t offset = 0;
                if (!lower_memory_address(lower, instr->operands[1],
                                          &ptr, &offset)) {
                    return false;
                }

                int64_t imm = 0;
                if (ppc_get_wide_const_value(lower, instr->operands[0], &imm)) {
                    return ppc_lower_store_i64_const(lower, imm, ptr, offset);
                }
                if (wide_pair_get(lower, instr->operands[0],
                                  NULL, NULL, NULL)) {
                    return ppc_lower_store_i64_pair(lower, instr->operands[0],
                                                    ptr, offset);
                }
            }

            anvil_mir_vreg_t val = lower_value(lower, instr->operands[0]);
            anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
            int64_t offset = 0;
            if (!lower_memory_address(lower, instr->operands[1],
                                      &ptr, &offset)) {
                return false;
            }
            if (val == ANVIL_MIR_NO_VREG || ptr == ANVIL_MIR_NO_VREG) {
                return false;
            }
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
            if (!lower_phi_copies_for_edge(lower, instr->parent,
                                           instr->true_block)) {
                return false;
            }
            return anvil_mir_add_branch(lower->mir, target);
        }
        case ANVIL_OP_BR_COND: {
            if (instr->num_operands != 1 || !instr->true_block ||
                !instr->false_block) {
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
            if (instr->num_operands == 1) {
                return add_return_copy(lower, instr->operands[0]);
            }
            return false;
        default:
            return false;
    }
}

anvil_mir_func_t *anvil_ppc_lower_func_to_mir(anvil_func_t *func,
                                              anvil_ppc_variant_t variant)
{
    const anvil_ppc_target_desc_t *desc = anvil_ppc_get_target_desc(variant);
    if (!desc || !func || func->is_declaration) return NULL;

    ppc_mir_lower_t lower;
    memset(&lower, 0, sizeof(lower));
    lower.desc = desc;
    lower.func = func;
    lower.mir = anvil_mir_func_create(func->name);
    if (!lower.mir) return NULL;

    if (!create_mir_blocks(&lower) ||
        !lower_params(&lower) ||
        !prepare_phi_results(&lower)) {
        anvil_mir_func_destroy(lower.mir);
        ppc_lower_free(&lower);
        return NULL;
    }

    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        anvil_mir_block_t mir_block = block_get(&lower, block);
        if (mir_block == ANVIL_MIR_NO_BLOCK ||
            !anvil_mir_set_current_block(lower.mir, mir_block)) {
            anvil_mir_func_destroy(lower.mir);
            ppc_lower_free(&lower);
            return NULL;
        }

        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (!lower_instr(&lower, instr)) {
                anvil_mir_func_destroy(lower.mir);
                ppc_lower_free(&lower);
                return NULL;
            }
        }
    }

    ppc_lower_free(&lower);
    return lower.mir;
}

static bool ppc_legal_fail(char *error, size_t error_len,
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

static bool ppc_reg_is_in_set(int reg, const int *regs, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (regs[i] == reg) return true;
    }
    return false;
}

static bool ppc_legal_size_for_class(const anvil_ppc_target_desc_t *desc,
                                     const anvil_mir_vreg_info_t *info)
{
    if (!desc || !info) return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR) {
        if (info->size_bits != 8 &&
            info->size_bits != 16 &&
            info->size_bits != 32 &&
            info->size_bits != 64) {
            return false;
        }
        return info->size_bits <= desc->word_size * 8;
    }
    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        return info->size_bits == 32 || info->size_bits == 64;
    }
    return false;
}

static bool ppc_legal_fixed_reg(const anvil_mir_vreg_info_t *info)
{
    if (!info || !info->has_fixed_reg) return true;
    if (info->fixed_phys_reg < 0) return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR) return info->fixed_phys_reg < 32;
    if (info->reg_class == ANVIL_MIR_REG_FPR) return info->fixed_phys_reg < 32;
    return false;
}

static const anvil_mir_vreg_info_t *ppc_legal_vreg_info(
    const anvil_ppc_target_desc_t *desc,
    const anvil_mir_func_t *mir,
    anvil_mir_vreg_t vreg,
    size_t instr_index,
    char *error,
    size_t error_len)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, vreg);
    if (!info) {
        ppc_legal_fail(error, error_len,
                       "PowerPC MIR instruction %zu uses invalid vreg",
                       instr_index);
        return NULL;
    }
    if (!ppc_legal_size_for_class(desc, info)) {
        ppc_legal_fail(error, error_len,
                       "PowerPC MIR instruction %zu uses unsupported vreg class/size",
                       instr_index);
        return NULL;
    }
    if (!ppc_legal_fixed_reg(info)) {
        ppc_legal_fail(error, error_len,
                       "PowerPC MIR instruction %zu uses invalid fixed register",
                       instr_index);
        return NULL;
    }
    return info;
}

static bool ppc_legal_pointer_operand(const anvil_ppc_target_desc_t *desc,
                                      const anvil_mir_func_t *mir,
                                      anvil_mir_vreg_t vreg,
                                      size_t instr_index,
                                      char *error,
                                      size_t error_len)
{
    const anvil_mir_vreg_info_t *info =
        ppc_legal_vreg_info(desc, mir, vreg, instr_index, error, error_len);
    if (!info) return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR &&
        info->size_bits == desc->word_size * 8) {
        return true;
    }
    return ppc_legal_fail(error, error_len,
                          "PowerPC MIR instruction %zu requires a pointer-sized operand",
                          instr_index);
}

static bool ppc_legal_same_class_and_size(const anvil_mir_vreg_info_t *a,
                                          const anvil_mir_vreg_info_t *b)
{
    return a && b &&
           a->reg_class == b->reg_class &&
           a->size_bits == b->size_bits;
}

static bool ppc_legal_numeric_conversion(
    size_t instr_index,
    const anvil_mir_instr_info_t *instr,
    const anvil_mir_vreg_info_t *dst,
    const anvil_mir_vreg_info_t *src,
    char *error,
    size_t error_len)
{
    bool legal = false;
    switch (instr->op) {
        case ANVIL_MIR_OP_SITOFP:
        case ANVIL_MIR_OP_UITOFP:
            legal = src->reg_class == ANVIL_MIR_REG_GPR &&
                    dst->reg_class == ANVIL_MIR_REG_FPR;
            break;
        case ANVIL_MIR_OP_FPTOSI:
        case ANVIL_MIR_OP_FPTOUI:
            legal = src->reg_class == ANVIL_MIR_REG_FPR &&
                    dst->reg_class == ANVIL_MIR_REG_GPR;
            break;
        case ANVIL_MIR_OP_FPEXT:
            legal = src->reg_class == ANVIL_MIR_REG_FPR &&
                    dst->reg_class == ANVIL_MIR_REG_FPR &&
                    src->size_bits == 32 && dst->size_bits == 64;
            break;
        case ANVIL_MIR_OP_FPTRUNC:
            legal = src->reg_class == ANVIL_MIR_REG_FPR &&
                    dst->reg_class == ANVIL_MIR_REG_FPR &&
                    src->size_bits == 64 && dst->size_bits == 32;
            break;
        default:
            break;
    }
    if (legal) return true;
    return ppc_legal_fail(error, error_len,
                          "PowerPC MIR conversion %zu has incompatible source/destination types",
                          instr_index);
}

static bool ppc_legal_binary(const anvil_ppc_target_desc_t *desc,
                             const anvil_mir_func_t *mir,
                             size_t instr_index,
                             const anvil_mir_instr_info_t *instr,
                             char *error,
                             size_t error_len)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(mir, instr_index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(mir, instr_index, 1);
    const anvil_mir_vreg_info_t *def =
        ppc_legal_vreg_info(desc, mir, instr->def, instr_index,
                            error, error_len);
    const anvil_mir_vreg_info_t *lhs_info =
        ppc_legal_vreg_info(desc, mir, lhs, instr_index, error, error_len);
    const anvil_mir_vreg_info_t *rhs_info =
        ppc_legal_vreg_info(desc, mir, rhs, instr_index, error, error_len);
    if (!def || !lhs_info || !rhs_info) return false;

    if (!ppc_legal_same_class_and_size(def, lhs_info) ||
        !ppc_legal_same_class_and_size(lhs_info, rhs_info)) {
        return ppc_legal_fail(error, error_len,
                              "PowerPC MIR instruction %zu has incompatible binary operands",
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
            return def->reg_class == ANVIL_MIR_REG_GPR;
        default:
            break;
    }

    return ppc_legal_fail(error, error_len,
                          "PowerPC MIR instruction %zu uses an illegal binary opcode/class pair",
                          instr_index);
}

static bool ppc_legal_call(const anvil_ppc_target_desc_t *desc,
                           const anvil_mir_func_t *mir,
                           size_t instr_index,
                           const anvil_mir_instr_info_t *instr,
                           char *error,
                           size_t error_len)
{
    if (instr->def != ANVIL_MIR_NO_VREG) {
        const anvil_mir_vreg_info_t *def =
            ppc_legal_vreg_info(desc, mir, instr->def, instr_index,
                                error, error_len);
        if (!def) return false;
        int ret_reg = def->reg_class == ANVIL_MIR_REG_FPR
            ? desc->fpr_return_reg
            : desc->gpr_return_reg;
        if (!def->has_fixed_reg || def->fixed_phys_reg != ret_reg) {
            return ppc_legal_fail(error, error_len,
                                  "PowerPC MIR call %zu result must be fixed to ABI result register",
                                  instr_index);
        }
    }

    size_t arg_start = 0;
    if (!instr->symbol || !instr->symbol[0]) {
        if (instr->num_uses == 0) {
            return ppc_legal_fail(error, error_len,
                                  "PowerPC MIR indirect call %zu requires a target register",
                                  instr_index);
        }

        anvil_mir_vreg_t target = anvil_mir_get_instr_use(mir, instr_index, 0);
        const anvil_mir_vreg_info_t *target_info =
            ppc_legal_vreg_info(desc, mir, target, instr_index,
                                error, error_len);
        if (!target_info) return false;
        if (target_info->reg_class != ANVIL_MIR_REG_GPR ||
            target_info->size_bits != desc->word_size * 8 ||
            !target_info->has_fixed_reg ||
            target_info->fixed_phys_reg != desc->indirect_call_reg) {
            return ppc_legal_fail(error, error_len,
                                  "PowerPC MIR indirect call %zu target must be fixed to ABI linkage register",
                                  instr_index);
        }
        arg_start = 1;
    }

    for (size_t u = arg_start; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, instr_index, u);
        const anvil_mir_vreg_info_t *info =
            ppc_legal_vreg_info(desc, mir, use, instr_index,
                                error, error_len);
        if (!info) return false;
        if (!info->has_fixed_reg) {
            return ppc_legal_fail(error, error_len,
                                  "PowerPC MIR call %zu argument %zu must use a fixed ABI register",
                                  instr_index, u - arg_start);
        }
        if (info->reg_class == ANVIL_MIR_REG_GPR &&
            ppc_reg_is_in_set(info->fixed_phys_reg, desc->gpr_arg_regs,
                              desc->num_gpr_arg_regs)) {
            continue;
        }
        if (info->reg_class == ANVIL_MIR_REG_FPR &&
            ppc_reg_is_in_set(info->fixed_phys_reg, desc->fpr_arg_regs,
                              desc->num_fpr_arg_regs)) {
            continue;
        }
        return ppc_legal_fail(error, error_len,
                              "PowerPC MIR call %zu argument %zu has an invalid fixed ABI register",
                              instr_index, u - arg_start);
    }

    return true;
}

static bool ppc_legal_instr(const anvil_ppc_target_desc_t *desc,
                            const anvil_mir_func_t *mir,
                            size_t instr_index,
                            const anvil_mir_instr_info_t *instr,
                            char *error,
                            size_t error_len)
{
    if (instr->def != ANVIL_MIR_NO_VREG &&
        !ppc_legal_vreg_info(desc, mir, instr->def, instr_index,
                             error, error_len)) {
        return false;
    }
    for (size_t u = 0; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, instr_index, u);
        if (!ppc_legal_vreg_info(desc, mir, use, instr_index,
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
            return ppc_legal_binary(desc, mir, instr_index, instr,
                                    error, error_len);

        case ANVIL_MIR_OP_LOAD: {
            anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(mir, instr_index, 0);
            return ppc_legal_pointer_operand(desc, mir, ptr, instr_index,
                                             error, error_len);
        }
        case ANVIL_MIR_OP_STORE: {
            anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(mir, instr_index, 1);
            return ppc_legal_pointer_operand(desc, mir, ptr, instr_index,
                                             error, error_len);
        }
        case ANVIL_MIR_OP_SYMBOL_ADDR:
        case ANVIL_MIR_OP_FRAME_ADDR:
        case ANVIL_MIR_OP_DYN_ALLOCA: {
            const anvil_mir_vreg_info_t *def =
                ppc_legal_vreg_info(desc, mir, instr->def, instr_index,
                                    error, error_len);
            if (def && def->reg_class == ANVIL_MIR_REG_GPR &&
                def->size_bits == desc->word_size * 8) {
                return true;
            }
            return ppc_legal_fail(error, error_len,
                                  "PowerPC MIR instruction %zu must define a pointer-sized GPR",
                                  instr_index);
        }
        case ANVIL_MIR_OP_INCOMING_STACK_ARG:
        case ANVIL_MIR_OP_CALL_STACK_ARG:
            if (instr->has_imm && instr->imm >= 0 &&
                (instr->imm % (int64_t)desc->word_size) == 0) {
                return true;
            }
            return ppc_legal_fail(error, error_len,
                                  "PowerPC MIR stack instruction %zu needs a word-aligned stack offset",
                                  instr_index);
        case ANVIL_MIR_OP_CALL:
            return ppc_legal_call(desc, mir, instr_index, instr,
                                  error, error_len);
        case ANVIL_MIR_OP_SELECT: {
            const anvil_mir_vreg_info_t *def =
                ppc_legal_vreg_info(desc, mir, instr->def, instr_index,
                                    error, error_len);
            const anvil_mir_vreg_info_t *then_info =
                ppc_legal_vreg_info(
                    desc, mir, anvil_mir_get_instr_use(mir, instr_index, 1),
                    instr_index, error, error_len);
            const anvil_mir_vreg_info_t *else_info =
                ppc_legal_vreg_info(
                    desc, mir, anvil_mir_get_instr_use(mir, instr_index, 2),
                    instr_index, error, error_len);
            if (ppc_legal_same_class_and_size(def, then_info) &&
                ppc_legal_same_class_and_size(then_info, else_info)) {
                return true;
            }
            return ppc_legal_fail(error, error_len,
                                  "PowerPC MIR select %zu has incompatible value operands",
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
            return true;
        case ANVIL_MIR_OP_SITOFP:
        case ANVIL_MIR_OP_UITOFP:
        case ANVIL_MIR_OP_FPTOSI:
        case ANVIL_MIR_OP_FPTOUI:
        case ANVIL_MIR_OP_FPEXT:
        case ANVIL_MIR_OP_FPTRUNC: {
            if (instr->num_uses != 1) {
                return ppc_legal_fail(error, error_len,
                                      "PowerPC MIR conversion %zu needs one source",
                                      instr_index);
            }
            const anvil_mir_vreg_info_t *dst =
                ppc_legal_vreg_info(desc, mir, instr->def, instr_index,
                                    error, error_len);
            const anvil_mir_vreg_info_t *src = ppc_legal_vreg_info(
                desc, mir, anvil_mir_get_instr_use(mir, instr_index, 0),
                instr_index, error, error_len);
            return dst && src && ppc_legal_numeric_conversion(
                instr_index, instr, dst, src, error, error_len);
        }
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
            return true;
        case ANVIL_MIR_OP_CALL_RESULT: {
            const anvil_mir_vreg_info_t *def = instr->def != ANVIL_MIR_NO_VREG
                ? ppc_legal_vreg_info(desc, mir, instr->def, instr_index,
                                      error, error_len)
                : NULL;
            if (!instr->symbol && instr->num_uses == 0 && def &&
                def->reg_class == ANVIL_MIR_REG_GPR &&
                def->size_bits == 32 && def->has_fixed_reg &&
                def->fixed_phys_reg == 4 && desc->word_size == 4) {
                return true;
            }
            return ppc_legal_fail(error, error_len,
                                  "PowerPC MIR instruction %zu has an invalid call-result pseudo",
                                  instr_index);
        }
        case ANVIL_MIR_OP_KEEPALIVE:
            return true;
        case ANVIL_MIR_OP_RET_VALUE_PART:
            return ppc_legal_fail(error, error_len,
                                  "PowerPC MIR instruction %zu uses an unsupported pseudo",
                                  instr_index);
        case ANVIL_MIR_OP_INVALID:
        default:
            break;
    }

    return ppc_legal_fail(error, error_len,
                          "PowerPC MIR instruction %zu uses unsupported opcode",
                          instr_index);
}

bool anvil_ppc_verify_mir_legal(const anvil_mir_func_t *mir,
                                anvil_ppc_variant_t variant,
                                char *error,
                                size_t error_len)
{
    const anvil_ppc_target_desc_t *desc = anvil_ppc_get_target_desc(variant);
    if (error && error_len > 0) error[0] = '\0';
    if (!desc) {
        return ppc_legal_fail(error, error_len, "unknown PowerPC variant");
    }
    if (!anvil_mir_verify(mir, error, error_len)) return false;

    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, i, &instr)) {
            return ppc_legal_fail(error, error_len,
                                  "PowerPC MIR instruction %zu is not inspectable",
                                  i);
        }
        if (!ppc_legal_instr(desc, mir, i, &instr, error, error_len)) {
            return false;
        }
    }

    return true;
}

bool anvil_ppc_regalloc_mir(anvil_mir_func_t *mir,
                            anvil_ppc_variant_t variant)
{
    const anvil_ppc_target_desc_t *desc = anvil_ppc_get_target_desc(variant);
    if (!desc || !mir) return false;

    anvil_regalloc_class_config_t configs[] = {
        {
            ANVIL_MIR_REG_GPR,
            (int)desc->num_alloc_gpr_regs,
            desc->alloc_gpr_regs
        },
        {
            ANVIL_MIR_REG_FPR,
            (int)desc->num_alloc_fpr_regs,
            desc->alloc_fpr_regs
        },
    };
    anvil_regalloc_class_config_t scratch_configs[] = {
        {
            ANVIL_MIR_REG_GPR,
            (int)desc->num_scratch_gpr_regs,
            desc->scratch_gpr_regs
        },
        {
            ANVIL_MIR_REG_FPR,
            (int)desc->num_scratch_fpr_regs,
            desc->scratch_fpr_regs
        },
    };

    if (!anvil_ppc_verify_mir_legal(mir, variant, NULL, 0)) return false;
    if (!anvil_mir_coalesce_copies(mir)) return false;
    if (!anvil_ppc_verify_mir_legal(mir, variant, NULL, 0)) return false;
    if (!anvil_regalloc_linear_scan_classes(
            mir, configs, sizeof(configs) / sizeof(configs[0]))) {
        return false;
    }
    if (!anvil_mir_materialize_spills(
            mir, scratch_configs,
            sizeof(scratch_configs) / sizeof(scratch_configs[0]))) {
        return false;
    }
    return anvil_ppc_verify_mir_legal(mir, variant, NULL, 0);
}

typedef struct {
    const anvil_ppc_target_desc_t *desc;
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
    int fp_const_scratch_offset;
    size_t label_counter;
    bool has_frame;
    bool failed;
} ppc_mir_emit_t;

static const char *ppc_gpr_names[] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23",
    "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31"
};

static const char *ppc_fpr_names[] = {
    "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7",
    "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15",
    "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23",
    "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31"
};

static int align_int(int value, int align)
{
    return (value + align - 1) & ~(align - 1);
}

static int ppc_mir_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0) return 8;
    int size = (int)((size_bits + 7) / 8);
    if (size <= 0) return 8;
    if (size > 8) return 8;
    return size;
}

static int ppc_mir_slot_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0) return 8;
    int size = (int)((size_bits + 7) / 8);
    return size > 0 ? size : 8;
}

static bool ppc_is_64(const ppc_mir_emit_t *emit)
{
    return emit->desc->word_size == 8;
}

static const char *ppc_store_word_op(const ppc_mir_emit_t *emit)
{
    return ppc_is_64(emit) ? "std" : "stw";
}

static const char *ppc_load_word_op(const ppc_mir_emit_t *emit)
{
    return ppc_is_64(emit) ? "ld" : "lwz";
}

static const anvil_mir_vreg_info_t *ppc_vreg_info_checked(
    ppc_mir_emit_t *emit,
    anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(emit->mir, vreg);
    if (!info) emit->failed = true;
    return info;
}

static const anvil_regalloc_assignment_t *ppc_assignment_checked(
    ppc_mir_emit_t *emit,
    anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *assignment =
        anvil_mir_get_assignment(emit->mir, vreg);
    if (!assignment || assignment->spilled || assignment->phys_reg < 0 ||
        assignment->phys_reg >= 32) {
        emit->failed = true;
        return NULL;
    }
    return assignment;
}

static const char *ppc_reg_name(ppc_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = ppc_vreg_info_checked(emit, vreg);
    const anvil_regalloc_assignment_t *assignment =
        ppc_assignment_checked(emit, vreg);
    if (!info || !assignment) return "?";
    return info->reg_class == ANVIL_MIR_REG_FPR
        ? ppc_fpr_names[assignment->phys_reg]
        : ppc_gpr_names[assignment->phys_reg];
}

static bool ppc_offset_fits_dform(int64_t offset)
{
    return offset >= -32768 && offset <= 32767;
}

static void ppc_emit_load_imm(ppc_mir_emit_t *emit, int reg, int64_t imm)
{
    if (reg < 0 || reg >= 32) {
        emit->failed = true;
        return;
    }

    const char *r = ppc_gpr_names[reg];
    if (imm >= -32768 && imm <= 32767) {
        anvil_strbuf_appendf(&emit->code, "\tli %s, %lld\n",
                             r, (long long)imm);
        return;
    }

    if (!ppc_is_64(emit) ||
        (imm >= INT32_MIN && imm <= INT32_MAX)) {
        uint32_t raw = (uint32_t)imm;
        uint32_t hi = (raw >> 16) & 0xffffu;
        uint32_t lo = raw & 0xffffu;
        anvil_strbuf_appendf(&emit->code, "\tlis %s, %u\n", r, hi);
        if (lo != 0) {
            anvil_strbuf_appendf(&emit->code, "\tori %s, %s, %u\n",
                                 r, r, lo);
        }
        return;
    }

    uint64_t raw = (uint64_t)imm;
    uint16_t c0 = (uint16_t)((raw >> 48) & 0xffffu);
    uint16_t c1 = (uint16_t)((raw >> 32) & 0xffffu);
    uint16_t c2 = (uint16_t)((raw >> 16) & 0xffffu);
    uint16_t c3 = (uint16_t)(raw & 0xffffu);
    anvil_strbuf_appendf(&emit->code, "\tlis %s, %u\n", r, (unsigned)c0);
    anvil_strbuf_appendf(&emit->code, "\tori %s, %s, %u\n", r, r, (unsigned)c1);
    anvil_strbuf_appendf(&emit->code, "\tsldi %s, %s, 32\n", r, r);
    if (c2 != 0) {
        anvil_strbuf_appendf(&emit->code, "\toris %s, %s, %u\n",
                             r, r, (unsigned)c2);
    }
    if (c3 != 0) {
        anvil_strbuf_appendf(&emit->code, "\tori %s, %s, %u\n",
                             r, r, (unsigned)c3);
    }
}

static void ppc_emit_addi_large(ppc_mir_emit_t *emit,
                                int dst,
                                int base,
                                int64_t offset)
{
    if (ppc_offset_fits_dform(offset)) {
        anvil_strbuf_appendf(&emit->code, "\taddi %s, %s, %lld\n",
                             ppc_gpr_names[dst], ppc_gpr_names[base],
                             (long long)offset);
        return;
    }

    ppc_emit_load_imm(emit, 11, offset);
    anvil_strbuf_appendf(&emit->code, "\tadd %s, %s, r11\n",
                         ppc_gpr_names[dst], ppc_gpr_names[base]);
}

static void ppc_emit_local_access(ppc_mir_emit_t *emit,
                                  const char *op,
                                  const char *reg,
                                  int offset)
{
    if (offset < 0) {
        emit->failed = true;
        return;
    }

    int64_t dform = -(int64_t)offset;
    if (ppc_offset_fits_dform(dform)) {
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %lld(r31)\n",
                             op, reg, (long long)dform);
        return;
    }

    ppc_emit_load_imm(emit, 11, dform);
    anvil_strbuf_append(&emit->code, "\tadd r11, r31, r11\n");
    anvil_strbuf_appendf(&emit->code, "\t%s %s, 0(r11)\n", op, reg);
}

static void ppc_emit_sp_access(ppc_mir_emit_t *emit,
                               const char *op,
                               const char *reg,
                               int offset)
{
    if (offset < 0) {
        emit->failed = true;
        return;
    }

    if (ppc_offset_fits_dform(offset)) {
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %d(r1)\n",
                             op, reg, offset);
        return;
    }

    ppc_emit_load_imm(emit, 11, offset);
    anvil_strbuf_append(&emit->code, "\tadd r11, r1, r11\n");
    anvil_strbuf_appendf(&emit->code, "\t%s %s, 0(r11)\n", op, reg);
}

static void ppc_emit_incoming_stack_access(ppc_mir_emit_t *emit,
                                           const char *op,
                                           const char *reg,
                                           int offset)
{
    if (offset < 0) {
        emit->failed = true;
        return;
    }

    if (ppc_offset_fits_dform(offset)) {
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %d(r31)\n",
                             op, reg, offset);
        return;
    }

    ppc_emit_load_imm(emit, 11, offset);
    anvil_strbuf_append(&emit->code, "\tadd r11, r31, r11\n");
    anvil_strbuf_appendf(&emit->code, "\t%s %s, 0(r11)\n", op, reg);
}

static const char *ppc_load_op(const anvil_ppc_target_desc_t *desc,
                               anvil_mir_reg_class_t reg_class,
                               int size,
                               bool is_signed)
{
    if (reg_class == ANVIL_MIR_REG_FPR) return size <= 4 ? "lfs" : "lfd";
    if (is_signed) {
        switch (size) {
            case 1: return "lbz";
            case 2: return "lha";
            case 4: return desc->word_size == 8 ? "lwa" : "lwz";
            default: return "ld";
        }
    }
    switch (size) {
        case 1: return "lbz";
        case 2: return "lhz";
        case 4: return "lwz";
        default: return "ld";
    }
}

static const char *ppc_store_op(const anvil_ppc_target_desc_t *desc,
                                anvil_mir_reg_class_t reg_class,
                                int size)
{
    (void)desc;
    if (reg_class == ANVIL_MIR_REG_FPR) return size <= 4 ? "stfs" : "stfd";
    switch (size) {
        case 1: return "stb";
        case 2: return "sth";
        case 4: return "stw";
        default: return "std";
    }
}

static void ppc_emit_sign_extend_loaded_byte(ppc_mir_emit_t *emit,
                                             const anvil_mir_vreg_info_t *info,
                                             const char *reg,
                                             int size)
{
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR || !info->is_signed) return;
    if (size == 1) {
        anvil_strbuf_appendf(&emit->code, "\textsb %s, %s\n", reg, reg);
    }
}

static void ppc_emit_base_offset_access(ppc_mir_emit_t *emit,
                                        const char *op,
                                        const char *reg,
                                        const char *base,
                                        int64_t offset)
{
    if (ppc_offset_fits_dform(offset)) {
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %lld(%s)\n",
                             op, reg, (long long)offset, base);
        return;
    }

    ppc_emit_load_imm(emit, 11, offset);
    anvil_strbuf_appendf(&emit->code, "\tadd r11, %s, r11\n", base);
    anvil_strbuf_appendf(&emit->code, "\t%s %s, 0(r11)\n", op, reg);
}

static bool ppc_emit_label(ppc_mir_emit_t *emit, anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info)) return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s:\n",
                         anvil_mir_func_name(emit->mir), name);
    return true;
}

static bool ppc_emit_branch_target(ppc_mir_emit_t *emit,
                                   anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info)) return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s",
                         anvil_mir_func_name(emit->mir), name);
    return true;
}

static bool ppc_instr_has_call(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info)) return true;
        switch (info.op) {
            case ANVIL_MIR_OP_CALL:
            case ANVIL_MIR_OP_SITOFP:
            case ANVIL_MIR_OP_UITOFP:
            case ANVIL_MIR_OP_FPTOSI:
            case ANVIL_MIR_OP_FPTOUI:
                return true;
            default:
                break;
        }
    }
    return false;
}

static bool ppc_instr_has_dynamic_alloca(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info)) return true;
        if (info.op == ANVIL_MIR_OP_DYN_ALLOCA) return true;
    }
    return false;
}

static bool ppc_instr_has_incoming_stack_arg(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info)) return true;
        if (info.op == ANVIL_MIR_OP_INCOMING_STACK_ARG) return true;
    }
    return false;
}

static bool ppc_instr_needs_fp_scratch(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, i, &instr)) return true;

        if (instr.def == ANVIL_MIR_NO_VREG) {
            continue;
        }
        const anvil_mir_vreg_info_t *def_info =
            anvil_mir_get_vreg_info(mir, instr.def);
        if (!def_info) return true;

        if (instr.op == ANVIL_MIR_OP_MOV && instr.has_imm &&
            def_info->reg_class == ANVIL_MIR_REG_FPR) {
            return true;
        }

        if ((instr.op == ANVIL_MIR_OP_COPY ||
             instr.op == ANVIL_MIR_OP_BITCAST) &&
            instr.num_uses == 1) {
            anvil_mir_vreg_t src = anvil_mir_get_instr_use(mir, i, 0);
            const anvil_mir_vreg_info_t *src_info =
                anvil_mir_get_vreg_info(mir, src);
            if (!src_info) return true;
            if (src_info->reg_class != def_info->reg_class &&
                (src_info->reg_class == ANVIL_MIR_REG_FPR ||
                 def_info->reg_class == ANVIL_MIR_REG_FPR)) {
                return true;
            }
        }
    }
    return false;
}

static bool ppc_scan_outgoing_stack_args(ppc_mir_emit_t *emit)
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

        int slot_size = align_int(ppc_mir_size_bytes(arg_info->size_bits),
                                  (int)emit->desc->word_size);
        if (slot_size < (int)emit->desc->word_size) {
            slot_size = (int)emit->desc->word_size;
        }
        int end = (int)info.imm + slot_size;
        if (end > outgoing_size) outgoing_size = end;
    }

    emit->outgoing_size = align_int(outgoing_size, 16);
    return true;
}

static bool ppc_prepare_frame(ppc_mir_emit_t *emit)
{
    for (size_t i = 0; i < 32; i++) {
        emit->gpr_save_offsets[i] = -1;
        emit->fpr_save_offsets[i] = -1;
    }
    emit->fp_const_scratch_offset = -1;

    if (!ppc_scan_outgoing_stack_args(emit)) return false;

    int word = (int)emit->desc->word_size;
    int offset = word;
    emit->gpr_save_offsets[31] = offset;

    for (size_t i = 0; i < anvil_mir_num_vregs(emit->mir); i++) {
        const anvil_regalloc_assignment_t *assignment =
            anvil_mir_get_assignment(emit->mir, (anvil_mir_vreg_t)i);
        if (!assignment || assignment->spilled) continue;

        if (assignment->reg_class == ANVIL_MIR_REG_GPR &&
            assignment->phys_reg >= 14 && assignment->phys_reg <= 30 &&
            emit->gpr_save_offsets[assignment->phys_reg] < 0) {
            offset += word;
            emit->gpr_save_offsets[assignment->phys_reg] = offset;
        } else if (assignment->reg_class == ANVIL_MIR_REG_FPR &&
                   assignment->phys_reg >= 14 && assignment->phys_reg <= 31 &&
                   emit->fpr_save_offsets[assignment->phys_reg] < 0) {
            offset = align_int(offset, 8);
            offset += 8;
            emit->fpr_save_offsets[assignment->phys_reg] = offset;
        }
    }

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
        int align = slot.align_bytes ? slot.align_bytes : word;
        if (align > 16) align = 16;
        offset = align_int(offset, align);
        offset += ppc_mir_slot_size_bytes(slot.size_bits);
        emit->frame_slot_offsets[i] = offset;
    }

    emit->num_spill_offsets = anvil_mir_num_spills(emit->mir);
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
        offset = align_int(offset, ppc_mir_size_bytes(slot.size_bits));
        offset += ppc_mir_size_bytes(slot.size_bits);
        emit->spill_offsets[i] = offset;
    }

    if (ppc_instr_needs_fp_scratch(emit->mir)) {
        offset = align_int(offset, 8);
        offset += 8;
        emit->fp_const_scratch_offset = offset;
    }

    int outgoing_end = (int)emit->desc->outgoing_arg_offset + emit->outgoing_size;
    int needed = offset + outgoing_end;
    if (needed < outgoing_end) return false;

    emit->has_frame = needed > word ||
                      emit->outgoing_size > 0 ||
                      ppc_instr_has_call(emit->mir) ||
                      ppc_instr_has_dynamic_alloca(emit->mir) ||
                      ppc_instr_has_incoming_stack_arg(emit->mir) ||
                      ppc_instr_needs_fp_scratch(emit->mir);
    if (!emit->has_frame) {
        emit->frame_size = 0;
        return true;
    }

    if (needed < (int)emit->desc->min_frame_size) {
        needed = (int)emit->desc->min_frame_size;
    }
    emit->frame_size = align_int(needed, 16);
    return true;
}

static void ppc_emit_function_header(ppc_mir_emit_t *emit)
{
    const char *name = anvil_mir_func_name(emit->mir);
    if (emit->desc->variant == ANVIL_PPC_VARIANT_PPC64) {
        anvil_strbuf_append(&emit->code, "\t.abiversion 1\n");
        anvil_strbuf_append(&emit->code, "\t.section \".opd\",\"aw\"\n");
        anvil_strbuf_append(&emit->code, "\t.align 3\n");
        anvil_strbuf_appendf(&emit->code, "\t.globl %s\n", name);
        anvil_strbuf_appendf(&emit->code, "%s:\n", name);
        anvil_strbuf_appendf(&emit->code, "\t.quad .L.%s,.TOC.@tocbase,0\n",
                             name);
        anvil_strbuf_append(&emit->code, "\t.previous\n");
        anvil_strbuf_appendf(&emit->code, "\t.type %s, @function\n", name);
        anvil_strbuf_appendf(&emit->code, ".L.%s:\n", name);
        return;
    }

    if (emit->desc->variant == ANVIL_PPC_VARIANT_PPC64LE) {
        anvil_strbuf_append(&emit->code, "\t.abiversion 2\n");
        anvil_strbuf_append(&emit->code, "\t.text\n");
        anvil_strbuf_appendf(&emit->code, "\t.globl %s\n", name);
        anvil_strbuf_appendf(&emit->code, "\t.type %s, @function\n", name);
        anvil_strbuf_appendf(&emit->code, "%s:\n", name);
        anvil_strbuf_append(&emit->code,
                            "0:\taddis r2, r12, (.TOC.-0b)@ha\n");
        anvil_strbuf_append(&emit->code, "\taddi r2, r2, (.TOC.-0b)@l\n");
        anvil_strbuf_appendf(&emit->code, "\t.localentry %s, .-0b\n", name);
        return;
    }

    anvil_strbuf_append(&emit->code, "\t.text\n");
    anvil_strbuf_appendf(&emit->code, "\t.globl %s\n", name);
    anvil_strbuf_appendf(&emit->code, "\t.type %s, @function\n", name);
    anvil_strbuf_appendf(&emit->code, "%s:\n", name);
}

static void ppc_emit_frame_adjust(ppc_mir_emit_t *emit,
                                  const char *op,
                                  int frame_size)
{
    if (frame_size <= 32767) {
        anvil_strbuf_appendf(&emit->code, "\t%s r1, %s%d(r1)\n",
                             ppc_is_64(emit) ? "stdu" : "stwu",
                             op[0] == '-' ? "-" : "",
                             frame_size);
        return;
    }

    ppc_emit_load_imm(emit, 11, frame_size);
    anvil_strbuf_append(&emit->code, "\tneg r11, r11\n");
    anvil_strbuf_appendf(&emit->code, "\t%s r1, r1, r11\n",
                         ppc_is_64(emit) ? "stdux" : "stwux");
}

static void ppc_emit_prologue(ppc_mir_emit_t *emit)
{
    ppc_emit_function_header(emit);
    if (!emit->has_frame) return;

    anvil_strbuf_append(&emit->code, "\tmflr r0\n");
    anvil_strbuf_appendf(&emit->code, "\t%s r0, %u(r1)\n",
                         ppc_store_word_op(emit), emit->desc->lr_save_offset);
    if (emit->desc->uses_function_descriptors && emit->desc->toc_save_offset > 0) {
        anvil_strbuf_appendf(&emit->code, "\tstd r2, %u(r1)\n",
                             emit->desc->toc_save_offset);
    }

    ppc_emit_frame_adjust(emit, "-", emit->frame_size);

    int r31_offset = emit->frame_size - emit->gpr_save_offsets[31];
    anvil_strbuf_appendf(&emit->code, "\t%s r31, %d(r1)\n",
                         ppc_store_word_op(emit), r31_offset);
    ppc_emit_addi_large(emit, 31, 1, emit->frame_size);

    for (int reg = 14; reg <= 30; reg++) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            ppc_emit_local_access(emit, ppc_store_word_op(emit),
                                  ppc_gpr_names[reg],
                                  emit->gpr_save_offsets[reg]);
        }
    }
    for (int reg = 14; reg <= 31; reg++) {
        if (emit->fpr_save_offsets[reg] >= 0) {
            ppc_emit_local_access(emit, "stfd", ppc_fpr_names[reg],
                                  emit->fpr_save_offsets[reg]);
        }
    }
}

static void ppc_emit_stack_restore(ppc_mir_emit_t *emit)
{
    if (emit->frame_size <= 32767) {
        anvil_strbuf_appendf(&emit->code, "\taddi r1, r1, %d\n",
                             emit->frame_size);
        return;
    }

    ppc_emit_load_imm(emit, 11, emit->frame_size);
    anvil_strbuf_append(&emit->code, "\tadd r1, r1, r11\n");
}

static void ppc_emit_epilogue(ppc_mir_emit_t *emit)
{
    if (emit->has_frame) {
        for (int reg = 14; reg <= 31; reg++) {
            if (emit->fpr_save_offsets[reg] >= 0) {
                ppc_emit_local_access(emit, "lfd", ppc_fpr_names[reg],
                                      emit->fpr_save_offsets[reg]);
            }
        }
        for (int reg = 30; reg >= 14; reg--) {
            if (emit->gpr_save_offsets[reg] >= 0) {
                ppc_emit_local_access(emit, ppc_load_word_op(emit),
                                      ppc_gpr_names[reg],
                                      emit->gpr_save_offsets[reg]);
            }
        }

        if (emit->desc->uses_function_descriptors &&
            emit->desc->toc_save_offset > 0) {
            anvil_strbuf_appendf(&emit->code, "\tld r2, %u(r31)\n",
                                 emit->desc->toc_save_offset);
        }
        anvil_strbuf_appendf(&emit->code, "\t%s r0, %u(r31)\n",
                             ppc_load_word_op(emit),
                             emit->desc->lr_save_offset);
        anvil_strbuf_append(&emit->code, "\tmtlr r0\n");
        if (ppc_instr_has_dynamic_alloca(emit->mir)) {
            anvil_strbuf_append(&emit->code, "\tmr r1, r31\n");
        } else {
            ppc_emit_stack_restore(emit);
        }
        anvil_strbuf_appendf(&emit->code, "\t%s r31, -%u(r1)\n",
                             ppc_load_word_op(emit), emit->desc->word_size);
    }
    anvil_strbuf_append(&emit->code, "\tblr\n");
}

static bool ppc_get_uses2(const anvil_mir_func_t *mir,
                          size_t instr_index,
                          anvil_mir_vreg_t *lhs,
                          anvil_mir_vreg_t *rhs)
{
    *lhs = anvil_mir_get_instr_use(mir, instr_index, 0);
    *rhs = anvil_mir_get_instr_use(mir, instr_index, 1);
    return *lhs != ANVIL_MIR_NO_VREG && *rhs != ANVIL_MIR_NO_VREG;
}

static void ppc_emit_copy(ppc_mir_emit_t *emit,
                          anvil_mir_vreg_t dst,
                          anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *dst_info = ppc_vreg_info_checked(emit, dst);
    const anvil_mir_vreg_info_t *src_info = ppc_vreg_info_checked(emit, src);
    const anvil_regalloc_assignment_t *dst_assignment =
        ppc_assignment_checked(emit, dst);
    const anvil_regalloc_assignment_t *src_assignment =
        ppc_assignment_checked(emit, src);
    if (!dst_info || !src_info || !dst_assignment || !src_assignment) return;

    const char *dst_reg = ppc_reg_name(emit, dst);
    const char *src_reg = ppc_reg_name(emit, src);
    if (emit->failed) return;
    if (dst_assignment->phys_reg == src_assignment->phys_reg &&
        dst_info->reg_class == src_info->reg_class) {
        return;
    }

    if (dst_info->reg_class == ANVIL_MIR_REG_GPR &&
        src_info->reg_class == ANVIL_MIR_REG_GPR) {
        anvil_strbuf_appendf(&emit->code, "\tmr %s, %s\n", dst_reg, src_reg);
        return;
    }
    if (dst_info->reg_class == ANVIL_MIR_REG_FPR &&
        src_info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\tfmr %s, %s\n", dst_reg, src_reg);
        return;
    }

    if (!emit->has_frame || emit->fp_const_scratch_offset < 0) {
        emit->failed = true;
        return;
    }

    if (src_info->reg_class == ANVIL_MIR_REG_GPR &&
        dst_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = ppc_mir_size_bytes(dst_info->size_bits);
        ppc_emit_local_access(emit,
                              size <= 4 ? "stw" : ppc_store_word_op(emit),
                              src_reg, emit->fp_const_scratch_offset);
        ppc_emit_local_access(emit, size <= 4 ? "lfs" : "lfd",
                              dst_reg, emit->fp_const_scratch_offset);
        return;
    }

    if (src_info->reg_class == ANVIL_MIR_REG_FPR &&
        dst_info->reg_class == ANVIL_MIR_REG_GPR) {
        int size = ppc_mir_size_bytes(src_info->size_bits);
        ppc_emit_local_access(emit, size <= 4 ? "stfs" : "stfd",
                              src_reg, emit->fp_const_scratch_offset);
        ppc_emit_local_access(emit,
                              size <= 4 ? "lwz" : ppc_load_word_op(emit),
                              dst_reg, emit->fp_const_scratch_offset);
        return;
    }

    emit->failed = true;
}

static void ppc_emit_binary(ppc_mir_emit_t *emit,
                            size_t instr_index,
                            const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs;
    anvil_mir_vreg_t rhs;
    if (!ppc_get_uses2(emit->mir, instr_index, &lhs, &rhs)) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info =
        ppc_vreg_info_checked(emit, info->def);
    if (!def_info) return;

    const char *dst = ppc_reg_name(emit, info->def);
    const char *a = ppc_reg_name(emit, lhs);
    const char *b = ppc_reg_name(emit, rhs);
    if (emit->failed) return;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        const char *op = NULL;
        bool single = def_info->size_bits == 32;
        switch (info->op) {
            case ANVIL_MIR_OP_ADD: op = single ? "fadds" : "fadd"; break;
            case ANVIL_MIR_OP_SUB: op = single ? "fsubs" : "fsub"; break;
            case ANVIL_MIR_OP_MUL: op = single ? "fmuls" : "fmul"; break;
            case ANVIL_MIR_OP_DIV:
            case ANVIL_MIR_OP_FDIV: op = single ? "fdivs" : "fdiv"; break;
            default: break;
        }
        if (!op) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n",
                             op, dst, a, b);
        return;
    }

    bool wide = ppc_is_64(emit) && def_info->size_bits > 32;
    switch (info->op) {
        case ANVIL_MIR_OP_ADD:
            anvil_strbuf_appendf(&emit->code, "\tadd %s, %s, %s\n", dst, a, b);
            break;
        case ANVIL_MIR_OP_SUB:
            anvil_strbuf_appendf(&emit->code, "\tsubf %s, %s, %s\n", dst, b, a);
            break;
        case ANVIL_MIR_OP_MUL:
            anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n",
                                 wide ? "mulld" : "mullw", dst, a, b);
            break;
        case ANVIL_MIR_OP_DIV:
        case ANVIL_MIR_OP_SDIV:
            anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n",
                                 wide ? "divd" : "divw", dst, a, b);
            break;
        case ANVIL_MIR_OP_UDIV:
            anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n",
                                 wide ? "divdu" : "divwu", dst, a, b);
            break;
        case ANVIL_MIR_OP_SMOD:
            anvil_strbuf_appendf(&emit->code, "\t%s r11, %s, %s\n",
                                 wide ? "divd" : "divw", a, b);
            anvil_strbuf_appendf(&emit->code, "\t%s r11, r11, %s\n",
                                 wide ? "mulld" : "mullw", b);
            anvil_strbuf_appendf(&emit->code, "\tsubf %s, r11, %s\n", dst, a);
            break;
        case ANVIL_MIR_OP_UMOD:
            anvil_strbuf_appendf(&emit->code, "\t%s r11, %s, %s\n",
                                 wide ? "divdu" : "divwu", a, b);
            anvil_strbuf_appendf(&emit->code, "\t%s r11, r11, %s\n",
                                 wide ? "mulld" : "mullw", b);
            anvil_strbuf_appendf(&emit->code, "\tsubf %s, r11, %s\n", dst, a);
            break;
        case ANVIL_MIR_OP_AND:
            anvil_strbuf_appendf(&emit->code, "\tand %s, %s, %s\n", dst, a, b);
            break;
        case ANVIL_MIR_OP_OR:
            anvil_strbuf_appendf(&emit->code, "\tor %s, %s, %s\n", dst, a, b);
            break;
        case ANVIL_MIR_OP_XOR:
            anvil_strbuf_appendf(&emit->code, "\txor %s, %s, %s\n", dst, a, b);
            break;
        case ANVIL_MIR_OP_SHL:
            anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n",
                                 wide ? "sld" : "slw", dst, a, b);
            break;
        case ANVIL_MIR_OP_SHR:
            anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n",
                                 wide ? "srd" : "srw", dst, a, b);
            break;
        case ANVIL_MIR_OP_SAR:
            anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n",
                                 wide ? "srad" : "sraw", dst, a, b);
            break;
        default:
            emit->failed = true;
            break;
    }
}

static const char *ppc_cmp_branch(anvil_mir_opcode_t op)
{
    switch (op) {
        case ANVIL_MIR_OP_CMP_EQ: return "beq";
        case ANVIL_MIR_OP_CMP_NE:
        case ANVIL_MIR_OP_CMP: return "bne";
        case ANVIL_MIR_OP_CMP_LT:
        case ANVIL_MIR_OP_CMP_ULT: return "blt";
        case ANVIL_MIR_OP_CMP_LE:
        case ANVIL_MIR_OP_CMP_ULE: return "ble";
        case ANVIL_MIR_OP_CMP_GT:
        case ANVIL_MIR_OP_CMP_UGT: return "bgt";
        case ANVIL_MIR_OP_CMP_GE:
        case ANVIL_MIR_OP_CMP_UGE: return "bge";
        default: return "bne";
    }
}

static bool ppc_cmp_is_unsigned(anvil_mir_opcode_t op)
{
    return op == ANVIL_MIR_OP_CMP_ULT ||
           op == ANVIL_MIR_OP_CMP_ULE ||
           op == ANVIL_MIR_OP_CMP_UGT ||
           op == ANVIL_MIR_OP_CMP_UGE;
}

static void ppc_emit_cmp(ppc_mir_emit_t *emit,
                         size_t instr_index,
                         const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs;
    anvil_mir_vreg_t rhs;
    if (!ppc_get_uses2(emit->mir, instr_index, &lhs, &rhs)) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *lhs_info = ppc_vreg_info_checked(emit, lhs);
    const char *dst = ppc_reg_name(emit, info->def);
    const char *a = ppc_reg_name(emit, lhs);
    const char *b = ppc_reg_name(emit, rhs);
    if (!lhs_info || emit->failed) return;

    if (lhs_info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\tfcmpu 0, %s, %s\n", a, b);
    } else {
        bool wide = ppc_is_64(emit) && lhs_info->size_bits > 32;
        bool unsigned_cmp = ppc_cmp_is_unsigned(info->op);
        anvil_strbuf_appendf(&emit->code, "\t%s 0, %s, %s\n",
                             unsigned_cmp
                                 ? (wide ? "cmpld" : "cmplw")
                                 : (wide ? "cmpd" : "cmpw"),
                             a, b);
    }

    size_t id = emit->label_counter++;
    anvil_strbuf_appendf(&emit->code, "\tli %s, 0\n", dst);
    if (info->op == ANVIL_MIR_OP_FCMP) {
        unsigned mask = 0;
        switch ((anvil_fcmp_pred_t)info->imm) {
            case ANVIL_FCMP_FALSE: mask = 0; break;
            case ANVIL_FCMP_OEQ: mask = 4; break;
            case ANVIL_FCMP_OGT: mask = 2; break;
            case ANVIL_FCMP_OGE: mask = 2|4; break;
            case ANVIL_FCMP_OLT: mask = 1; break;
            case ANVIL_FCMP_OLE: mask = 1|4; break;
            case ANVIL_FCMP_ONE: mask = 1|2; break;
            case ANVIL_FCMP_ORD: mask = 1|2|4; break;
            case ANVIL_FCMP_UEQ: mask = 4|8; break;
            case ANVIL_FCMP_UGT: mask = 2|8; break;
            case ANVIL_FCMP_UGE: mask = 2|4|8; break;
            case ANVIL_FCMP_ULT: mask = 1|8; break;
            case ANVIL_FCMP_ULE: mask = 1|4|8; break;
            case ANVIL_FCMP_UNE: mask = 1|2|8; break;
            case ANVIL_FCMP_UNO: mask = 8; break;
            case ANVIL_FCMP_TRUE: mask = 15; break;
        }
        const char *branches[] = { "blt", "bgt", "beq", "bun" };
        for (unsigned bit = 0; bit < 4; bit++) if (mask & (1u << bit))
            anvil_strbuf_appendf(&emit->code, "\t%s .L%s_cmp_true_%zu\n",
                branches[bit], anvil_mir_func_name(emit->mir), id);
    } else {
        anvil_strbuf_appendf(&emit->code, "\t%s .L%s_cmp_true_%zu\n",
                             ppc_cmp_branch(info->op),
                             anvil_mir_func_name(emit->mir), id);
    }
    anvil_strbuf_appendf(&emit->code, "\tb .L%s_cmp_done_%zu\n",
                         anvil_mir_func_name(emit->mir), id);
    anvil_strbuf_appendf(&emit->code, ".L%s_cmp_true_%zu:\n",
                         anvil_mir_func_name(emit->mir), id);
    anvil_strbuf_appendf(&emit->code, "\tli %s, 1\n", dst);
    anvil_strbuf_appendf(&emit->code, ".L%s_cmp_done_%zu:\n",
                         anvil_mir_func_name(emit->mir), id);
}

static void ppc_emit_unary(ppc_mir_emit_t *emit,
                           size_t instr_index,
                           const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info =
        ppc_vreg_info_checked(emit, info->def);
    const char *dst = ppc_reg_name(emit, info->def);
    const char *s = ppc_reg_name(emit, src);
    if (!def_info || emit->failed) return;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        switch (info->op) {
            case ANVIL_MIR_OP_NEG:
                anvil_strbuf_appendf(&emit->code, "\tfneg %s, %s\n", dst, s);
                break;
            case ANVIL_MIR_OP_FABS:
                anvil_strbuf_appendf(&emit->code, "\tfabs %s, %s\n", dst, s);
                break;
            default:
                emit->failed = true;
                break;
        }
        return;
    }

    switch (info->op) {
        case ANVIL_MIR_OP_NEG:
            anvil_strbuf_appendf(&emit->code, "\tneg %s, %s\n", dst, s);
            break;
        case ANVIL_MIR_OP_NOT:
            anvil_strbuf_appendf(&emit->code, "\tnor %s, %s, %s\n", dst, s, s);
            break;
        default:
            emit->failed = true;
            break;
    }
}

static void ppc_emit_frame_addr(ppc_mir_emit_t *emit,
                                const anvil_mir_instr_info_t *info)
{
    if (!emit->has_frame || info->frame_slot < 0 ||
        (size_t)info->frame_slot >= emit->num_frame_slot_offsets) {
        emit->failed = true;
        return;
    }

    const anvil_regalloc_assignment_t *assignment =
        ppc_assignment_checked(emit, info->def);
    if (!assignment) return;
    ppc_emit_addi_large(emit, assignment->phys_reg, 31,
                        -emit->frame_slot_offsets[info->frame_slot]);
}

static void ppc_emit_dyn_alloca(ppc_mir_emit_t *emit,
                                size_t instr_index,
                                const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t count = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (count == ANVIL_MIR_NO_VREG || !info->has_imm || info->imm <= 0) {
        emit->failed = true;
        return;
    }

    const anvil_regalloc_assignment_t *dst_assignment =
        ppc_assignment_checked(emit, info->def);
    const char *count_reg = ppc_reg_name(emit, count);
    if (!dst_assignment || emit->failed) return;

    if (info->imm == 1) {
        anvil_strbuf_appendf(&emit->code, "\tmr r11, %s\n", count_reg);
    } else if (info->imm >= -32768 && info->imm <= 32767) {
        anvil_strbuf_appendf(&emit->code, "\tmulli r11, %s, %lld\n",
                             count_reg, (long long)info->imm);
    } else {
        ppc_emit_load_imm(emit, 11, info->imm);
        anvil_strbuf_appendf(&emit->code, "\t%s r11, r11, %s\n",
                             ppc_is_64(emit) ? "mulld" : "mullw",
                             count_reg);
    }
    /* The dynamic frame must expose a complete ABI linkage/outgoing area to
       nested calls.  Keep user payload beyond that aligned prefix. */
    int prefix = (int)emit->desc->outgoing_arg_offset + emit->outgoing_size;
    if (prefix < (int)emit->desc->min_frame_size) {
        prefix = (int)emit->desc->min_frame_size;
    }
    prefix = align_int(prefix, 16);
    if (prefix <= 32752) {
        anvil_strbuf_appendf(&emit->code, "\taddi r11, r11, %d\n",
                             prefix + 15);
    } else {
        ppc_emit_load_imm(emit, 0, prefix + 15);
        anvil_strbuf_append(&emit->code, "\tadd r11, r11, r0\n");
    }
    anvil_strbuf_append(&emit->code, ppc_is_64(emit)
        ? "\tclrrdi r11, r11, 4\n"
        : "\trlwinm r11, r11, 0, 0, 27\n");
    anvil_strbuf_append(&emit->code, "\tneg r11, r11\n");
    anvil_strbuf_appendf(&emit->code, "\t%s r1, r1, r11\n",
                         ppc_is_64(emit) ? "stdux" : "stwux");
    ppc_emit_addi_large(emit, dst_assignment->phys_reg, 1, prefix);
}

static void ppc_emit_mov_fpr_imm(ppc_mir_emit_t *emit,
                                 const anvil_mir_instr_info_t *info,
                                 const anvil_mir_vreg_info_t *def_info,
                                 const char *dst)
{
    if (!emit->has_frame || emit->fp_const_scratch_offset < 0) {
        emit->failed = true;
        return;
    }

    uint64_t raw = (uint64_t)info->imm;
    int size = ppc_mir_size_bytes(def_info->size_bits);
    if (size <= 4) {
        ppc_emit_load_imm(emit, 11, (int32_t)(raw & 0xffffffffu));
        ppc_emit_local_access(emit, "stw", "r11",
                              emit->fp_const_scratch_offset);
        ppc_emit_local_access(emit, "lfs", dst,
                              emit->fp_const_scratch_offset);
        return;
    }

    if (ppc_is_64(emit)) {
        ppc_emit_load_imm(emit, 11, (int64_t)raw);
        ppc_emit_local_access(emit, "std", "r11",
                              emit->fp_const_scratch_offset);
        ppc_emit_local_access(emit, "lfd", dst,
                              emit->fp_const_scratch_offset);
        return;
    }

    uint32_t hi = (uint32_t)(raw >> 32);
    uint32_t lo = (uint32_t)(raw & 0xffffffffu);
    ppc_emit_load_imm(emit, 11, (int32_t)hi);
    ppc_emit_local_access(emit, "stw", "r11",
                          emit->fp_const_scratch_offset);
    ppc_emit_load_imm(emit, 11, (int32_t)lo);
    ppc_emit_local_access(emit, "stw", "r11",
                          emit->fp_const_scratch_offset - 4);
    ppc_emit_local_access(emit, "lfd", dst,
                          emit->fp_const_scratch_offset);
}

static void ppc_emit_mov(ppc_mir_emit_t *emit,
                         const anvil_mir_instr_info_t *info)
{
    if (!info->has_imm) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info =
        ppc_vreg_info_checked(emit, info->def);
    const anvil_regalloc_assignment_t *assignment =
        ppc_assignment_checked(emit, info->def);
    const char *dst = ppc_reg_name(emit, info->def);
    if (!def_info || !assignment || emit->failed) return;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        ppc_emit_mov_fpr_imm(emit, info, def_info, dst);
        return;
    }

    ppc_emit_load_imm(emit, assignment->phys_reg, info->imm);
}

static void ppc_emit_symbol_addr(ppc_mir_emit_t *emit,
                                 const anvil_mir_instr_info_t *info)
{
    if (!info->symbol || !info->symbol[0]) {
        emit->failed = true;
        return;
    }

    const anvil_regalloc_assignment_t *assignment =
        ppc_assignment_checked(emit, info->def);
    if (!assignment) return;
    const char *dst = ppc_gpr_names[assignment->phys_reg];

    if (ppc_is_64(emit)) {
        anvil_strbuf_appendf(&emit->code, "\taddis %s, r2, %s@toc@ha\n",
                             dst, info->symbol);
        anvil_strbuf_appendf(&emit->code, "\taddi %s, %s, %s@toc@l\n",
                             dst, dst, info->symbol);
    } else {
        anvil_strbuf_appendf(&emit->code, "\tlis %s, %s@ha\n",
                             dst, info->symbol);
        anvil_strbuf_appendf(&emit->code, "\taddi %s, %s, %s@l\n",
                             dst, dst, info->symbol);
    }
}

static void ppc_emit_load(ppc_mir_emit_t *emit,
                          size_t instr_index,
                          const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
    const char *dst = ppc_reg_name(emit, info->def);
    const char *base = ppc_reg_name(emit, ptr);
    if (!def_info || emit->failed) return;

    int size = ppc_mir_size_bytes(def_info->size_bits);
    const char *op = ppc_load_op(emit->desc, def_info->reg_class, size,
                                 def_info->is_signed);
    ppc_emit_base_offset_access(emit, op, dst, base,
                                info->has_imm ? info->imm : 0);
    ppc_emit_sign_extend_loaded_byte(emit, def_info, dst, size);
}

static void ppc_emit_store(ppc_mir_emit_t *emit, size_t instr_index)
{
    anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (value == ANVIL_MIR_NO_VREG || ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *value_info = ppc_vreg_info_checked(emit, value);
    const char *src = ppc_reg_name(emit, value);
    const char *base = ppc_reg_name(emit, ptr);
    if (!value_info || emit->failed) return;

    anvil_mir_instr_info_t info;
    int64_t offset = 0;
    if (anvil_mir_get_instr_info(emit->mir, instr_index, &info) &&
        info.has_imm) {
        offset = info.imm;
    }

    int size = ppc_mir_size_bytes(value_info->size_bits);
    ppc_emit_base_offset_access(
        emit,
        ppc_store_op(emit->desc, value_info->reg_class, size),
        src, base, offset);
}

static void ppc_emit_incoming_stack_arg(ppc_mir_emit_t *emit,
                                        const anvil_mir_instr_info_t *info)
{
    if (!emit->has_frame || !info->has_imm || info->imm < 0 ||
        info->imm > INT32_MAX) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
    const char *dst = ppc_reg_name(emit, info->def);
    if (!def_info || emit->failed) return;

    int size = ppc_mir_size_bytes(def_info->size_bits);
    int frame_offset = (int)emit->desc->incoming_arg_offset + (int)info->imm;
    ppc_emit_incoming_stack_access(
        emit,
        ppc_load_op(emit->desc, def_info->reg_class, size, def_info->is_signed),
        dst,
        frame_offset);
    ppc_emit_sign_extend_loaded_byte(emit, def_info, dst, size);
}

static void ppc_emit_gpr_extend(ppc_mir_emit_t *emit,
                                size_t instr_index,
                                const anvil_mir_instr_info_t *info,
                                bool sign_extend)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = ppc_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = ppc_vreg_info_checked(emit, src);
    const char *dst = ppc_reg_name(emit, info->def);
    const char *s = ppc_reg_name(emit, src);
    if (!dst_info || !src_info || emit->failed) return;
    if (dst_info->reg_class != ANVIL_MIR_REG_GPR ||
        src_info->reg_class != ANVIL_MIR_REG_GPR) {
        emit->failed = true;
        return;
    }

    int src_size = ppc_mir_size_bytes(src_info->size_bits);
    int dst_size = ppc_mir_size_bytes(dst_info->size_bits);
    if (sign_extend) {
        if (src_size <= 1) {
            anvil_strbuf_appendf(&emit->code, "\textsb %s, %s\n", dst, s);
        } else if (src_size <= 2) {
            anvil_strbuf_appendf(&emit->code, "\textsh %s, %s\n", dst, s);
        } else if (ppc_is_64(emit) && dst_size > 4 && src_size <= 4) {
            anvil_strbuf_appendf(&emit->code, "\textsw %s, %s\n", dst, s);
        } else {
            anvil_strbuf_appendf(&emit->code, "\tmr %s, %s\n", dst, s);
        }
        return;
    }

    if (src_size <= 1) {
        anvil_strbuf_appendf(&emit->code, "\trlwinm %s, %s, 0, 24, 31\n", dst, s);
    } else if (src_size <= 2) {
        anvil_strbuf_appendf(&emit->code, "\trlwinm %s, %s, 0, 16, 31\n", dst, s);
    } else if (ppc_is_64(emit) && dst_size > 4 && src_size <= 4) {
        anvil_strbuf_appendf(&emit->code, "\trldicl %s, %s, 0, 32\n", dst, s);
    } else {
        anvil_strbuf_appendf(&emit->code, "\tmr %s, %s\n", dst, s);
    }
}

static void ppc_emit_direct_call(ppc_mir_emit_t *emit, const char *symbol);

static void ppc_emit_cast(ppc_mir_emit_t *emit,
                          size_t instr_index,
                          const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = ppc_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = ppc_vreg_info_checked(emit, src);
    if (!dst_info || !src_info) return;

    const char *dst = ppc_reg_name(emit, info->def);
    const char *source = ppc_reg_name(emit, src);
    if (emit->failed) return;

    switch (info->op) {
        case ANVIL_MIR_OP_ZEXT:
            ppc_emit_gpr_extend(emit, instr_index, info, false);
            break;
        case ANVIL_MIR_OP_SEXT:
            ppc_emit_gpr_extend(emit, instr_index, info, true);
            break;
        case ANVIL_MIR_OP_TRUNC:
        case ANVIL_MIR_OP_BITCAST:
            ppc_emit_copy(emit, info->def, src);
            break;
        case ANVIL_MIR_OP_SITOFP:
        case ANVIL_MIR_OP_UITOFP: {
            bool is_unsigned = info->op == ANVIL_MIR_OP_UITOFP;
            if (src_info->size_bits <= 8) {
                anvil_strbuf_appendf(&emit->code,
                    is_unsigned ? "\trlwinm r3, %s, 0, 24, 31\n"
                                : "\textsb r3, %s\n", source);
            } else if (src_info->size_bits <= 16) {
                anvil_strbuf_appendf(&emit->code,
                    is_unsigned ? "\trlwinm r3, %s, 0, 16, 31\n"
                                : "\textsh r3, %s\n", source);
            } else if (src_info->size_bits <= 32 && ppc_is_64(emit)) {
                anvil_strbuf_appendf(&emit->code,
                    is_unsigned ? "\trldicl r3, %s, 0, 32\n"
                                : "\textsw r3, %s\n", source);
            } else if (strcmp(source, "r3") != 0) {
                anvil_strbuf_appendf(&emit->code, "\tmr r3, %s\n", source);
            }

            const char *helper;
            if (is_unsigned) {
                helper = src_info->size_bits <= 32
                    ? (dst_info->size_bits == 32 ? "__floatunsisf" : "__floatunsidf")
                    : (dst_info->size_bits == 32 ? "__floatundisf" : "__floatundidf");
            } else {
                helper = src_info->size_bits <= 32
                    ? (dst_info->size_bits == 32 ? "__floatsisf" : "__floatsidf")
                    : (dst_info->size_bits == 32 ? "__floatdisf" : "__floatdidf");
            }
            ppc_emit_direct_call(emit, helper);
            if (strcmp(dst, "f1") != 0) {
                anvil_strbuf_appendf(&emit->code, "\tfmr %s, f1\n", dst);
            }
            break;
        }
        case ANVIL_MIR_OP_FPTOSI:
        case ANVIL_MIR_OP_FPTOUI: {
            bool is_unsigned = info->op == ANVIL_MIR_OP_FPTOUI;
            if (strcmp(source, "f1") != 0) {
                anvil_strbuf_appendf(&emit->code, "\tfmr f1, %s\n", source);
            }
            const char *helper;
            if (is_unsigned) {
                helper = dst_info->size_bits <= 32
                    ? (src_info->size_bits == 32 ? "__fixunssfsi" : "__fixunsdfsi")
                    : (src_info->size_bits == 32 ? "__fixunssfdi" : "__fixunsdfdi");
            } else {
                helper = dst_info->size_bits <= 32
                    ? (src_info->size_bits == 32 ? "__fixsfsi" : "__fixdfsi")
                    : (src_info->size_bits == 32 ? "__fixsfdi" : "__fixdfdi");
            }
            ppc_emit_direct_call(emit, helper);

            if (dst_info->size_bits <= 8) {
                anvil_strbuf_appendf(&emit->code,
                    is_unsigned ? "\trlwinm %s, r3, 0, 24, 31\n"
                                : "\textsb %s, r3\n", dst);
            } else if (dst_info->size_bits <= 16) {
                anvil_strbuf_appendf(&emit->code,
                    is_unsigned ? "\trlwinm %s, r3, 0, 16, 31\n"
                                : "\textsh %s, r3\n", dst);
            } else if (dst_info->size_bits <= 32 && ppc_is_64(emit)) {
                anvil_strbuf_appendf(&emit->code,
                    is_unsigned ? "\trldicl %s, r3, 0, 32\n"
                                : "\textsw %s, r3\n", dst);
            } else if (strcmp(dst, "r3") != 0) {
                anvil_strbuf_appendf(&emit->code, "\tmr %s, r3\n", dst);
            }
            break;
        }
        case ANVIL_MIR_OP_FPEXT:
            /* PowerPC FPRs hold a loaded f32 as its numeric value in the
             * register's wider representation, so extending it is a move. */
            ppc_emit_copy(emit, info->def, src);
            break;
        case ANVIL_MIR_OP_FPTRUNC:
            anvil_strbuf_appendf(&emit->code, "\tfrsp %s, %s\n", dst, source);
            break;
        default:
            emit->failed = true;
            break;
    }
}

static void ppc_emit_select(ppc_mir_emit_t *emit,
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

    const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
    const char *cond_reg = ppc_reg_name(emit, cond);
    if (!def_info || emit->failed) return;

    size_t id = emit->label_counter++;
    anvil_strbuf_appendf(&emit->code, "\tcmpwi 0, %s, 0\n", cond_reg);
    anvil_strbuf_appendf(&emit->code, "\tbeq .L%s_select_else_%zu\n",
                         anvil_mir_func_name(emit->mir), id);
    ppc_emit_copy(emit, info->def, then_v);
    anvil_strbuf_appendf(&emit->code, "\tb .L%s_select_done_%zu\n",
                         anvil_mir_func_name(emit->mir), id);
    anvil_strbuf_appendf(&emit->code, ".L%s_select_else_%zu:\n",
                         anvil_mir_func_name(emit->mir), id);
    ppc_emit_copy(emit, info->def, else_v);
    anvil_strbuf_appendf(&emit->code, ".L%s_select_done_%zu:\n",
                         anvil_mir_func_name(emit->mir), id);
}

static void ppc_emit_call_stack_arg(ppc_mir_emit_t *emit,
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

    const anvil_mir_vreg_info_t *value_info = ppc_vreg_info_checked(emit, value);
    const char *src = ppc_reg_name(emit, value);
    if (!value_info || emit->failed) return;

    int size = ppc_mir_size_bytes(value_info->size_bits);
    int offset = (int)emit->desc->outgoing_arg_offset + (int)info->imm;
    ppc_emit_sp_access(emit,
                       ppc_store_op(emit->desc, value_info->reg_class, size),
                       src, offset);
}

static void ppc_emit_spill_load(ppc_mir_emit_t *emit,
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

    const char *dst = ppc_reg_name(emit, info->def);
    if (emit->failed) return;
    int size = ppc_mir_size_bytes(slot.size_bits);
    ppc_emit_local_access(emit,
                          ppc_load_op(emit->desc, slot.reg_class, size, false),
                          dst,
                          emit->spill_offsets[info->spill_slot]);
}

static void ppc_emit_spill_store(ppc_mir_emit_t *emit,
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

    const char *src = ppc_reg_name(emit, src_vreg);
    if (emit->failed) return;
    int size = ppc_mir_size_bytes(slot.size_bits);
    ppc_emit_local_access(emit,
                          ppc_store_op(emit->desc, slot.reg_class, size),
                          src,
                          emit->spill_offsets[info->spill_slot]);
}

static void ppc_emit_ret(ppc_mir_emit_t *emit,
                         size_t instr_index,
                         const anvil_mir_instr_info_t *info)
{
    if (info->num_uses > 0) {
        anvil_mir_vreg_t ret = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        const anvil_mir_vreg_info_t *ret_info = ppc_vreg_info_checked(emit, ret);
        const anvil_regalloc_assignment_t *assignment =
            ppc_assignment_checked(emit, ret);
        if (!ret_info || !assignment) return;

        int ret_reg = ret_info->reg_class == ANVIL_MIR_REG_FPR
            ? emit->desc->fpr_return_reg
            : emit->desc->gpr_return_reg;
        if (assignment->phys_reg != ret_reg) {
            if (ret_info->reg_class == ANVIL_MIR_REG_FPR) {
                anvil_strbuf_appendf(&emit->code, "\tfmr %s, %s\n",
                                     ppc_fpr_names[ret_reg],
                                     ppc_fpr_names[assignment->phys_reg]);
            } else {
                anvil_strbuf_appendf(&emit->code, "\tmr %s, %s\n",
                                     ppc_gpr_names[ret_reg],
                                     ppc_gpr_names[assignment->phys_reg]);
            }
        }
    }
    if (!emit->failed) ppc_emit_epilogue(emit);
}

static void ppc_emit_direct_call(ppc_mir_emit_t *emit, const char *symbol)
{
    anvil_strbuf_appendf(&emit->code, "\tbl %s\n", symbol);
    if (emit->desc->uses_function_descriptors &&
        emit->desc->toc_save_offset > 0 &&
        emit->has_frame) {
        anvil_strbuf_appendf(&emit->code, "\tld r2, %u(r31)\n",
                             emit->desc->toc_save_offset);
    }
}

static void ppc_emit_indirect_call(ppc_mir_emit_t *emit,
                                   size_t instr_index,
                                   const anvil_mir_instr_info_t *info)
{
    if (info->num_uses == 0) {
        emit->failed = true;
        return;
    }

    anvil_mir_vreg_t target =
        anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (target == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const char *target_reg = ppc_reg_name(emit, target);
    if (emit->failed) return;

    if (emit->desc->uses_function_descriptors) {
        anvil_strbuf_appendf(&emit->code, "\tld r11, 0(%s)\n", target_reg);
        anvil_strbuf_appendf(&emit->code, "\tld r2, 8(%s)\n", target_reg);
        anvil_strbuf_append(&emit->code, "\tmtctr r11\n");
    } else {
        anvil_strbuf_appendf(&emit->code, "\tmtctr %s\n", target_reg);
    }
    anvil_strbuf_append(&emit->code, "\tbctrl\n");

    if (emit->desc->uses_function_descriptors &&
        emit->desc->toc_save_offset > 0 &&
        emit->has_frame) {
        anvil_strbuf_appendf(&emit->code, "\tld r2, %u(r31)\n",
                             emit->desc->toc_save_offset);
    }
}

static void ppc_emit_instr(ppc_mir_emit_t *emit,
                           size_t instr_index,
                           const anvil_mir_instr_info_t *info)
{
    switch (info->op) {
        case ANVIL_MIR_OP_MOV:
            ppc_emit_mov(emit, info);
            break;
        case ANVIL_MIR_OP_COPY: {
            anvil_mir_vreg_t src =
                anvil_mir_get_instr_use(emit->mir, instr_index, 0);
            if (src == ANVIL_MIR_NO_VREG) {
                emit->failed = true;
                break;
            }
            ppc_emit_copy(emit, info->def, src);
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
            ppc_emit_binary(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_NEG:
        case ANVIL_MIR_OP_NOT:
        case ANVIL_MIR_OP_FABS:
            ppc_emit_unary(emit, instr_index, info);
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
            ppc_emit_cast(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_SELECT:
            ppc_emit_select(emit, instr_index, info);
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
            ppc_emit_cmp(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_SYMBOL_ADDR:
            ppc_emit_symbol_addr(emit, info);
            break;
        case ANVIL_MIR_OP_LOAD:
            ppc_emit_load(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_STORE:
            ppc_emit_store(emit, instr_index);
            break;
        case ANVIL_MIR_OP_FRAME_ADDR:
            ppc_emit_frame_addr(emit, info);
            break;
        case ANVIL_MIR_OP_DYN_ALLOCA:
            ppc_emit_dyn_alloca(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_INCOMING_STACK_ARG:
            ppc_emit_incoming_stack_arg(emit, info);
            break;
        case ANVIL_MIR_OP_CALL:
            if (info->symbol && info->symbol[0]) {
                ppc_emit_direct_call(emit, info->symbol);
            } else {
                ppc_emit_indirect_call(emit, instr_index, info);
            }
            break;
        case ANVIL_MIR_OP_CALL_STACK_ARG:
            ppc_emit_call_stack_arg(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_BR:
            anvil_strbuf_append(&emit->code, "\tb ");
            if (!ppc_emit_branch_target(emit, info->true_block)) {
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
            const char *cond_reg = ppc_reg_name(emit, cond);
            if (emit->failed) break;
            anvil_strbuf_appendf(&emit->code, "\tcmpwi 0, %s, 0\n", cond_reg);
            anvil_strbuf_append(&emit->code, "\tbne ");
            if (!ppc_emit_branch_target(emit, info->true_block)) {
                emit->failed = true;
                break;
            }
            anvil_strbuf_append(&emit->code, "\n\tb ");
            if (!ppc_emit_branch_target(emit, info->false_block)) {
                emit->failed = true;
                break;
            }
            anvil_strbuf_append(&emit->code, "\n");
            break;
        }
        case ANVIL_MIR_OP_RET:
            ppc_emit_ret(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_SPILL_LOAD:
            ppc_emit_spill_load(emit, info);
            break;
        case ANVIL_MIR_OP_SPILL_STORE:
            ppc_emit_spill_store(emit, instr_index, info);
            break;
        case ANVIL_MIR_OP_CALL_RESULT:
            {
                const anvil_regalloc_assignment_t *assignment =
                    ppc_assignment_checked(emit, info->def);
                if (!assignment || assignment->phys_reg != 4 ||
                    emit->desc->word_size != 4) {
                    emit->failed = true;
                }
            }
            break;
        case ANVIL_MIR_OP_KEEPALIVE:
            break;
        case ANVIL_MIR_OP_RET_VALUE_PART:
            emit->failed = true;
            break;
        default:
            emit->failed = true;
            break;
    }
}

static void ppc_emit_escaped_string(anvil_strbuf_t *code, const char *value)
{
    anvil_strbuf_append(code, "\t.asciz \"");
    for (const char *p = value ? value : ""; *p; p++) {
        switch (*p) {
            case '\n': anvil_strbuf_append(code, "\\n"); break;
            case '\r': anvil_strbuf_append(code, "\\r"); break;
            case '\t': anvil_strbuf_append(code, "\\t"); break;
            case '\\': anvil_strbuf_append(code, "\\\\"); break;
            case '"': anvil_strbuf_append(code, "\\\""); break;
            default: anvil_strbuf_append_char(code, *p); break;
        }
    }
    anvil_strbuf_append(code, "\"\n");
}

static void ppc_emit_string_literals(ppc_mir_emit_t *emit)
{
    size_t count = anvil_mir_num_string_literals(emit->mir);
    if (count == 0) return;

    anvil_strbuf_append(&emit->code, "\t.section .rodata\n");
    for (size_t i = 0; i < count; i++) {
        anvil_mir_string_literal_info_t info;
        if (!anvil_mir_get_string_literal_info(emit->mir, i, &info) ||
            !info.label) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "%s:\n", info.label);
        ppc_emit_escaped_string(&emit->code, info.value);
    }
}

bool anvil_ppc_emit_mir(const anvil_mir_func_t *mir,
                        anvil_ppc_variant_t variant,
                        char **output,
                        size_t *len)
{
    if (!output) return false;
    *output = NULL;
    if (len) *len = 0;

    const anvil_ppc_target_desc_t *desc = anvil_ppc_get_target_desc(variant);
    if (!desc || !mir) return false;
    if (!anvil_ppc_verify_mir_legal(mir, variant, NULL, 0)) return false;

    ppc_mir_emit_t emit;
    memset(&emit, 0, sizeof(emit));
    emit.desc = desc;
    emit.mir = mir;
    anvil_strbuf_init(&emit.code);

    if (!ppc_prepare_frame(&emit)) {
        anvil_strbuf_destroy(&emit.code);
        free(emit.spill_offsets);
        free(emit.frame_slot_offsets);
        return false;
    }

    ppc_emit_prologue(&emit);

    for (size_t b = 0;
         b < anvil_mir_num_blocks(mir) && !emit.code.failed; b++) {
        if (!ppc_emit_label(&emit, (anvil_mir_block_t)b)) {
            emit.failed = true;
            break;
        }

        for (size_t instr_index = 0;
             instr_index < anvil_mir_num_instrs(mir); instr_index++) {
            anvil_mir_instr_info_t instr;
            if (!anvil_mir_get_instr_info(mir, instr_index, &instr)) {
                emit.failed = true;
                break;
            }
            if (instr.block != (anvil_mir_block_t)b) continue;
            ppc_emit_instr(&emit, instr_index, &instr);
            if (emit.failed || emit.code.failed) break;
        }
        if (emit.failed || emit.code.failed) break;
    }

    ppc_emit_string_literals(&emit);

    free(emit.spill_offsets);
    free(emit.frame_slot_offsets);

    if (emit.failed || emit.code.failed) {
        anvil_strbuf_destroy(&emit.code);
        return false;
    }

    *output = anvil_strbuf_detach(&emit.code, len);
    return *output != NULL;
}

static void ppc_emit_data_zero(anvil_strbuf_t *out, size_t size)
{
    if (size == 0) size = 1;
    anvil_strbuf_appendf(out, "\t.zero %zu\n", size);
}

static void ppc_emit_data_int(anvil_strbuf_t *out, size_t size, int64_t value)
{
    switch (size) {
        case 1:
            anvil_strbuf_appendf(out, "\t.byte %lld\n", (long long)value);
            break;
        case 2:
            anvil_strbuf_appendf(out, "\t.short %lld\n", (long long)value);
            break;
        case 4:
            anvil_strbuf_appendf(out, "\t.long %lld\n", (long long)value);
            break;
        default:
            anvil_strbuf_appendf(out, "\t.quad %lld\n", (long long)value);
            break;
    }
}

static void ppc_emit_data_float(anvil_strbuf_t *out, size_t size, double value)
{
    if (size == 4) {
        float f = (float)value;
        uint32_t bits = 0;
        memcpy(&bits, &f, sizeof(bits));
        anvil_strbuf_appendf(out, "\t.long 0x%x\n", bits);
        return;
    }

    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    anvil_strbuf_appendf(out, "\t.quad 0x%llx\n",
                         (unsigned long long)bits);
}

static void ppc_emit_global_initializer(anvil_strbuf_t *out,
                                        anvil_type_t *type,
                                        anvil_value_t *init)
{
    size_t size = type ? anvil_type_size(type) : 0;
    if (size == 0) size = 1;

    if (!init) {
        ppc_emit_data_zero(out, size);
        return;
    }

    switch (init->kind) {
        case ANVIL_VAL_CONST_INT:
            ppc_emit_data_int(out, size, init->data.i);
            return;
        case ANVIL_VAL_CONST_NULL:
            ppc_emit_data_zero(out, size);
            return;
        case ANVIL_VAL_CONST_FLOAT:
            ppc_emit_data_float(out, size, init->data.f);
            return;
        case ANVIL_VAL_CONST_STRING:
            ppc_emit_escaped_string(out, init->data.str);
            return;
        case ANVIL_VAL_CONST_ARRAY:
            if (type && type->kind == ANVIL_TYPE_ARRAY && type->data.array.elem) {
                anvil_type_t *elem_type = type->data.array.elem;
                for (size_t i = 0; i < init->data.array.num_elements; i++) {
                    ppc_emit_global_initializer(out, elem_type,
                                                init->data.array.elements[i]);
                }
                size_t emitted = init->data.array.num_elements *
                                 anvil_type_size(elem_type);
                if (emitted < size) ppc_emit_data_zero(out, size - emitted);
                return;
            }
            break;
        case ANVIL_VAL_GLOBAL:
        case ANVIL_VAL_FUNC:
            if (init->name) {
                anvil_strbuf_appendf(out, "\t.%s %s\n",
                                     size <= 4 ? "long" : "quad",
                                     init->name);
                return;
            }
            break;
        default:
            break;
    }

    ppc_emit_data_zero(out, size);
}

static void ppc_emit_globals(anvil_strbuf_t *out, anvil_module_t *mod)
{
    if (!mod || mod->num_globals == 0) return;

    bool emitted_header = false;
    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        if (!g->value) continue;
        if (g->value->type && g->value->type->kind == ANVIL_TYPE_FUNC) {
            continue;
        }

        if (!emitted_header) {
            anvil_strbuf_append(out, "\t.data\n");
            emitted_header = true;
        }

        size_t align = g->value->type ? anvil_type_align(g->value->type) : 1;
        if (align == 0) align = 1;
        anvil_strbuf_appendf(out, "\t.globl %s\n", g->value->name);
        anvil_strbuf_appendf(out, "\t.align %zu\n", align);
        anvil_strbuf_appendf(out, "%s:\n", g->value->name);
        ppc_emit_global_initializer(out, g->value->type,
                                    g->value->data.global.init);
    }

    if (emitted_header) anvil_strbuf_append(out, "\n");
}

static const char *ppc_variant_display_name(anvil_ppc_variant_t variant)
{
    switch (variant) {
        case ANVIL_PPC_VARIANT_PPC32:
            return "PowerPC 32-bit";
        case ANVIL_PPC_VARIANT_PPC64:
            return "PowerPC 64-bit big-endian ELFv1";
        case ANVIL_PPC_VARIANT_PPC64LE:
            return "PowerPC 64-bit little-endian ELFv2";
        default:
            return "PowerPC";
    }
}

anvil_error_t anvil_ppc_codegen_func(anvil_backend_t *be,
                                     anvil_func_t *func,
                                     anvil_ppc_variant_t variant,
                                     char **output,
                                     size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    *output = NULL;
    if (len) *len = 0;

    if (func->is_declaration) {
        *output = calloc(1, 1);
        return *output ? ANVIL_OK : ANVIL_ERR_NOMEM;
    }

    if (be->ctx &&
        be->ctx->abi != ANVIL_ABI_SYSV &&
        be->ctx->abi != ANVIL_ABI_DEFAULT) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN,
                        "PowerPC MachineIR codegen supports the SysV ABI");
        return ANVIL_ERR_CODEGEN;
    }

    anvil_mir_func_t *mir = anvil_ppc_lower_func_to_mir(func, variant);
    if (!mir) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN,
                        "PowerPC MachineIR lowering failed for function %s",
                        func->name ? func->name : "<anonymous>");
        return ANVIL_ERR_CODEGEN;
    }

    bool ok = anvil_ppc_regalloc_mir(mir, variant);
    char *mir_text = NULL;
    size_t mir_len = 0;
    if (ok) ok = anvil_ppc_emit_mir(mir, variant, &mir_text, &mir_len);
    anvil_mir_func_destroy(mir);

    if (!ok || !mir_text) {
        free(mir_text);
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN,
                        "PowerPC MachineIR emission failed for function %s",
                        func->name ? func->name : "<anonymous>");
        return ANVIL_ERR_CODEGEN;
    }

    *output = mir_text;
    if (len) *len = mir_len;
    return ANVIL_OK;
}

anvil_error_t anvil_ppc_codegen_module(anvil_backend_t *be,
                                       anvil_module_t *mod,
                                       anvil_ppc_variant_t variant,
                                       char **output,
                                       size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;
    *output = NULL;
    if (len) *len = 0;

    const anvil_ppc_target_desc_t *desc = anvil_ppc_get_target_desc(variant);
    if (!desc) return ANVIL_ERR_INVALID_ARG;

    anvil_strbuf_t result;
    anvil_strbuf_init(&result);
    anvil_strbuf_appendf(&result, "# Generated by ANVIL for %s\n",
                         ppc_variant_display_name(variant));

    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (func->is_declaration) {
            anvil_strbuf_appendf(&result, "\t.extern %s\n", func->name);
        }
    }

    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (func->is_declaration) continue;

        char *func_text = NULL;
        size_t func_len = 0;
        anvil_error_t err =
            anvil_ppc_codegen_func(be, func, variant, &func_text, &func_len);
        if (err != ANVIL_OK) {
            anvil_strbuf_destroy(&result);
            free(func_text);
            return err;
        }
        if (func_text) {
            (void)func_len;
            anvil_strbuf_append(&result, func_text);
            anvil_strbuf_append(&result, "\n");
            free(func_text);
        }
    }

    ppc_emit_globals(&result, mod);

    (void)desc;
    if (result.failed) {
        anvil_strbuf_destroy(&result);
        return ANVIL_ERR_NOMEM;
    }
    *output = anvil_strbuf_detach(&result, len);
    return *output ? ANVIL_OK : ANVIL_ERR_NOMEM;
}
