#include "ppc_internal.h"

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

typedef anvil_mir_parallel_copy_t phi_edge_copy_t;

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
    if (!lower)
        return;
    free(lower->blocks);
    free(lower->values);
    free(lower->addr_offsets);
    free(lower->wide_consts);
    free(lower->wide_pairs);
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
    return type && (type->kind == ANVIL_TYPE_I64 || type->kind == ANVIL_TYPE_U64);
}

static bool ppc_needs_i64_pair(ppc_mir_lower_t *lower, anvil_type_t *type)
{
    return lower && lower->desc && lower->desc->word_size == 4 && ppc_type_is_64bit_integer(type);
}

static bool ppc_ensure_i64_pair(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *hi, anvil_mir_vreg_t *lo, bool *is_unsigned);

static anvil_mir_reg_class_t ppc_reg_class_for_type(anvil_type_t *type)
{
    return ppc_type_is_fp(type) ? ANVIL_MIR_REG_FPR : ANVIL_MIR_REG_GPR;
}

static uint16_t ppc_bits_for_type(const anvil_ppc_target_desc_t *desc, anvil_type_t *type)
{
    if (!desc)
        return 64;
    if (!type || type->kind == ANVIL_TYPE_PTR)
        return (uint16_t)(desc->word_size * 8);

    size_t size = anvil_type_size(type);
    if (size == 0)
        return (uint16_t)(desc->word_size * 8);
    if (size > UINT16_MAX / 8)
        return (uint16_t)(desc->word_size * 8);
    return (uint16_t)(size * 8);
}

static uint16_t ppc_slot_bits_for_type(const anvil_ppc_target_desc_t *desc, anvil_type_t *type)
{
    size_t size = type ? anvil_type_size(type) : desc->word_size;
    if (size == 0)
        size = desc->word_size;
    if (size > UINT16_MAX / 8)
        size = UINT16_MAX / 8;
    return (uint16_t)(size * 8);
}

static uint16_t ppc_align_for_type(const anvil_ppc_target_desc_t *desc, anvil_type_t *type)
{
    size_t align = type ? anvil_type_align(type) : desc->word_size;
    if (align == 0)
        align = desc->word_size;
    if (align > desc->word_size)
        align = desc->word_size;
    if (align > UINT16_MAX)
        align = UINT16_MAX;
    return (uint16_t)align;
}

static anvil_mir_vreg_t ppc_add_vreg_for_type(ppc_mir_lower_t *lower, anvil_type_t *type)
{
    return anvil_mir_add_vreg_typed(lower->mir, ppc_reg_class_for_type(type), ppc_bits_for_type(lower->desc, type), ppc_type_is_signed(type));
}

static bool map_reserve(ppc_mir_lower_t *lower, size_t needed)
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

static bool map_put(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t vreg)
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

static anvil_mir_vreg_t map_get(ppc_mir_lower_t *lower, anvil_value_t *value)
{
    for (size_t i = 0; i < lower->num_values; i++) {
        if (lower->values[i].value == value)
            return lower->values[i].vreg;
    }
    return ANVIL_MIR_NO_VREG;
}

static bool addr_map_reserve(ppc_mir_lower_t *lower, size_t needed)
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

static bool addr_map_put(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t base, int64_t offset)
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

static bool addr_map_get(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *out_base, int64_t *out_offset)
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

static bool wide_const_reserve(ppc_mir_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_wide_consts)
        return true;

    size_t new_cap = lower->cap_wide_consts ? lower->cap_wide_consts * 2 : 8;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2)
            return false;
        new_cap *= 2;
    }

    ppc_wide_const_t *grown = realloc(lower->wide_consts, new_cap * sizeof(*grown));
    if (!grown)
        return false;

    lower->wide_consts = grown;
    lower->cap_wide_consts = new_cap;
    return true;
}

static bool wide_const_put(ppc_mir_lower_t *lower, anvil_value_t *value, int64_t imm)
{
    if (!value)
        return false;

    for (size_t i = 0; i < lower->num_wide_consts; i++) {
        if (lower->wide_consts[i].value == value) {
            lower->wide_consts[i].imm = imm;
            return true;
        }
    }

    if (!wide_const_reserve(lower, lower->num_wide_consts + 1))
        return false;
    lower->wide_consts[lower->num_wide_consts].value = value;
    lower->wide_consts[lower->num_wide_consts].imm = imm;
    lower->num_wide_consts++;
    return true;
}

static bool wide_const_get(ppc_mir_lower_t *lower, anvil_value_t *value, int64_t *out)
{
    if (!value)
        return false;

    for (size_t i = 0; i < lower->num_wide_consts; i++) {
        if (lower->wide_consts[i].value == value) {
            if (out)
                *out = lower->wide_consts[i].imm;
            return true;
        }
    }
    return false;
}

static bool wide_pair_reserve(ppc_mir_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_wide_pairs)
        return true;

    size_t new_cap = lower->cap_wide_pairs ? lower->cap_wide_pairs * 2 : 8;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2)
            return false;
        new_cap *= 2;
    }

    ppc_wide_pair_t *grown = realloc(lower->wide_pairs, new_cap * sizeof(*grown));
    if (!grown)
        return false;

    lower->wide_pairs = grown;
    lower->cap_wide_pairs = new_cap;
    return true;
}

static bool wide_pair_put(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t hi, anvil_mir_vreg_t lo, bool is_unsigned)
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

    if (!wide_pair_reserve(lower, lower->num_wide_pairs + 1))
        return false;
    lower->wide_pairs[lower->num_wide_pairs].value = value;
    lower->wide_pairs[lower->num_wide_pairs].hi = hi;
    lower->wide_pairs[lower->num_wide_pairs].lo = lo;
    lower->wide_pairs[lower->num_wide_pairs].is_unsigned = is_unsigned;
    lower->num_wide_pairs++;
    return true;
}

static bool wide_pair_get(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *hi, anvil_mir_vreg_t *lo, bool *is_unsigned)
{
    if (!value)
        return false;

    for (size_t i = 0; i < lower->num_wide_pairs; i++) {
        if (lower->wide_pairs[i].value == value) {
            if (hi)
                *hi = lower->wide_pairs[i].hi;
            if (lo)
                *lo = lower->wide_pairs[i].lo;
            if (is_unsigned)
                *is_unsigned = lower->wide_pairs[i].is_unsigned;
            return true;
        }
    }
    return false;
}

static anvil_mir_block_t block_get(ppc_mir_lower_t *lower, anvil_block_t *block)
{
    if (!block)
        return ANVIL_MIR_NO_BLOCK;
    for (size_t i = 0; i < lower->num_blocks; i++) {
        if (lower->blocks[i].block == block)
            return lower->blocks[i].mir_block;
    }
    return ANVIL_MIR_NO_BLOCK;
}

static bool create_mir_blocks(ppc_mir_lower_t *lower)
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

static bool set_fixed_register_arg(ppc_mir_lower_t *lower, anvil_mir_vreg_t vreg, anvil_type_t *type, size_t *gpr_count, size_t *fpr_count)
{
    const anvil_ppc_target_desc_t *desc = lower->desc;
    if (ppc_type_is_fp(type)) {
        if (*fpr_count >= desc->num_fpr_arg_regs)
            return false;
        if (!anvil_mir_set_fixed_reg(lower->mir, vreg, desc->fpr_arg_regs[*fpr_count])) {
            return false;
        }
        (*fpr_count)++;
        return true;
    }

    if (*gpr_count >= desc->num_gpr_arg_regs)
        return false;
    if (!anvil_mir_set_fixed_reg(lower->mir, vreg, desc->gpr_arg_regs[*gpr_count])) {
        return false;
    }
    (*gpr_count)++;
    return true;
}

static bool arg_still_uses_register(ppc_mir_lower_t *lower, anvil_type_t *type, size_t gpr_count, size_t fpr_count)
{
    const anvil_ppc_target_desc_t *desc = lower->desc;
    return ppc_type_is_fp(type) ? fpr_count < desc->num_fpr_arg_regs : gpr_count < desc->num_gpr_arg_regs;
}

static void advance_arg_count(anvil_type_t *type, size_t *gpr_count, size_t *fpr_count)
{
    if (ppc_type_is_fp(type)) {
        (*fpr_count)++;
    } else {
        (*gpr_count)++;
    }
}

static int64_t ppc_stack_arg_slot_size(ppc_mir_lower_t *lower, anvil_type_t *type)
{
    int64_t size = type ? (int64_t)anvil_type_size(type) : (int64_t)lower->desc->word_size;
    int64_t word = (int64_t)lower->desc->word_size;
    if (size <= 0)
        size = word;
    if (size < word)
        size = word;
    return (size + word - 1) & ~(word - 1);
}

static bool lower_params(ppc_mir_lower_t *lower)
{
    size_t gpr_count = 0;
    size_t fpr_count = 0;
    int64_t stack_offset = 0;

    anvil_mir_block_t entry = block_get(lower, lower->func->blocks);
    if (entry == ANVIL_MIR_NO_BLOCK || !anvil_mir_set_current_block(lower->mir, entry)) {
        return false;
    }

    for (size_t i = 0; i < lower->func->num_params; i++) {
        anvil_value_t *param = lower->func->params[i];
        if (!param)
            return false;

        if (ppc_needs_i64_pair(lower, param->type)) {
            bool is_unsigned = param->type->kind == ANVIL_TYPE_U64;
            anvil_mir_vreg_t hi = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, !is_unsigned);
            anvil_mir_vreg_t lo = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, false);
            if (hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG) {
                return false;
            }

            if (gpr_count + 2 <= lower->desc->num_gpr_arg_regs) {
                anvil_mir_vreg_t incoming_hi = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, !is_unsigned);
                anvil_mir_vreg_t incoming_lo = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, false);
                if (incoming_hi == ANVIL_MIR_NO_VREG || incoming_lo == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->mir, incoming_hi, lower->desc->gpr_arg_regs[gpr_count]) ||
                    !anvil_mir_set_fixed_reg(lower->mir, incoming_lo, lower->desc->gpr_arg_regs[gpr_count + 1]) || !anvil_mir_set_live_in(lower->mir, incoming_hi, true) ||
                    !anvil_mir_set_live_in(lower->mir, incoming_lo, true)) {
                    return false;
                }
                anvil_mir_vreg_t hi_uses[] = {incoming_hi};
                anvil_mir_vreg_t lo_uses[] = {incoming_lo};
                if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, hi, hi_uses, 1) || !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, lo, lo_uses, 1)) {
                    return false;
                }
                gpr_count += 2;
            } else {
                if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, hi, stack_offset) ||
                    !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, lo, stack_offset + 4)) {
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
        if (local == ANVIL_MIR_NO_VREG)
            return false;

        if (arg_still_uses_register(lower, param->type, gpr_count, fpr_count)) {
            anvil_mir_vreg_t incoming = ppc_add_vreg_for_type(lower, param->type);
            if (incoming == ANVIL_MIR_NO_VREG)
                return false;
            if (!set_fixed_register_arg(lower, incoming, param->type, &gpr_count, &fpr_count) || !anvil_mir_set_live_in(lower->mir, incoming, true)) {
                return false;
            }

            anvil_mir_vreg_t uses[] = {incoming};
            if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, local, uses, 1)) {
                return false;
            }
        } else {
            if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, local, stack_offset)) {
                return false;
            }
            stack_offset += ppc_stack_arg_slot_size(lower, param->type);
            advance_arg_count(param->type, &gpr_count, &fpr_count);
        }

        if (!map_put(lower, param, local))
            return false;
    }

    return true;
}

static bool prepare_phi_results(ppc_mir_lower_t *lower)
{
    for (anvil_block_t *block = lower->func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_PHI)
                break;
            if (!instr->result)
                return false;

            anvil_mir_vreg_t vreg = ppc_add_vreg_for_type(lower, instr->result->type);
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

static int64_t ppc_int_constant(anvil_value_t *value)
{
    if (!value)
        return 0;
    if (value->type && !value->type->is_signed)
        return (int64_t)value->data.u;
    return value->data.i;
}

static bool ppc_get_const_int(anvil_value_t *value, int64_t *out)
{
    if (!value || value->kind != ANVIL_VAL_CONST_INT)
        return false;
    if (out)
        *out = ppc_int_constant(value);
    return true;
}

static anvil_mir_vreg_t lower_value(ppc_mir_lower_t *lower, anvil_value_t *value);
static bool lower_add_const_offset(ppc_mir_lower_t *lower, anvil_mir_vreg_t base, int64_t offset, anvil_mir_vreg_t *out_ptr);
static bool ppc_lower_i64_bitcast_pair(ppc_mir_lower_t *lower, anvil_instr_t *instr);

static anvil_mir_vreg_t lower_const_value(ppc_mir_lower_t *lower, anvil_value_t *value)
{
    anvil_mir_vreg_t vreg = ppc_add_vreg_for_type(lower, value->type);
    if (vreg == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;

    int64_t imm = 0;
    switch (value->kind) {
    case ANVIL_VAL_CONST_INT:
        imm = ppc_int_constant(value);
        break;
    case ANVIL_VAL_CONST_NULL:
        imm = 0;
        break;
    case ANVIL_VAL_CONST_FLOAT:
        imm = float_bits_as_i64(value->data.f, ppc_bits_for_type(lower->desc, value->type));
        break;
    default:
        return ANVIL_MIR_NO_VREG;
    }

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, vreg, imm)) {
        return ANVIL_MIR_NO_VREG;
    }
    return vreg;
}

static anvil_mir_vreg_t lower_symbol_address(ppc_mir_lower_t *lower, const char *symbol)
{
    if (!symbol || !symbol[0])
        return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t vreg = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->word_size * 8), false);
    if (vreg == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;

    if (!anvil_mir_add_instr_symbol(lower->mir, ANVIL_MIR_OP_SYMBOL_ADDR, vreg, NULL, 0, symbol)) {
        return ANVIL_MIR_NO_VREG;
    }
    return vreg;
}

static anvil_mir_vreg_t lower_reloc_address(ppc_mir_lower_t *lower, anvil_value_t *value)
{
    const char *symbol = value && value->data.reloc.symbol ? value->data.reloc.symbol->name : NULL;
    if (!symbol || !symbol[0])
        return ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t vreg = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->word_size * 8), false);
    if (vreg == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_symbol_imm(lower->mir, ANVIL_MIR_OP_SYMBOL_ADDR, vreg, NULL, 0, symbol, value->data.reloc.addend))
        return ANVIL_MIR_NO_VREG;
    return vreg;
}

static anvil_mir_vreg_t lower_string_address(ppc_mir_lower_t *lower, anvil_value_t *value)
{
    const char *label = NULL;
    if (anvil_mir_add_string_literal(lower->mir, value->data.str, &label) < 0 || !label) {
        return ANVIL_MIR_NO_VREG;
    }
    return lower_symbol_address(lower, label);
}

static anvil_mir_vreg_t lower_value(ppc_mir_lower_t *lower, anvil_value_t *value)
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

static bool add_return_copy(ppc_mir_lower_t *lower, anvil_value_t *value)
{
    if (!value) {
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, NULL, 0);
    }

    if (ppc_needs_i64_pair(lower, value->type)) {
        anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
        anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
        if (!wide_pair_get(lower, value, &hi, &lo, NULL))
            return false;
        const anvil_mir_vreg_info_t *hi_info = anvil_mir_get_vreg_info(lower->mir, hi);
        if (!hi_info)
            return false;
        anvil_mir_vreg_t ret_hi = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, hi_info->is_signed);
        anvil_mir_vreg_t ret_lo = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, false);
        if (ret_hi == ANVIL_MIR_NO_VREG || ret_lo == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->mir, ret_hi, 3) || !anvil_mir_set_fixed_reg(lower->mir, ret_lo, 4)) {
            return false;
        }
        anvil_mir_vreg_t hi_uses[] = {hi};
        anvil_mir_vreg_t lo_uses[] = {lo};
        anvil_mir_vreg_t ret_uses[] = {ret_hi};
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, ret_hi, hi_uses, 1) && anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, ret_lo, lo_uses, 1) &&
               anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, ret_uses, 1);
    }

    anvil_mir_vreg_t src = lower_value(lower, value);
    if (src == ANVIL_MIR_NO_VREG)
        return false;

    anvil_mir_vreg_t ret = ppc_add_vreg_for_type(lower, value->type);
    if (ret == ANVIL_MIR_NO_VREG)
        return false;
    if (!anvil_mir_set_fixed_reg(lower->mir, ret, ppc_type_is_fp(value->type) ? lower->desc->fpr_return_reg : lower->desc->gpr_return_reg)) {
        return false;
    }

    anvil_mir_vreg_t uses[] = {src};
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, ret, uses, 1)) {
        return false;
    }

    anvil_mir_vreg_t ret_uses[] = {ret};
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

static bool lower_phi_copies_for_edge(ppc_mir_lower_t *lower, anvil_block_t *src_block, anvil_block_t *dest_block)
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

        if (!found)
            goto fail;
    }

    bool ok = anvil_mir_emit_parallel_copies(lower->mir, copies, num_copies);
    free(copies);
    return ok;

fail:
    free(copies);
    return false;
}

static anvil_mir_block_t create_phi_edge_block(ppc_mir_lower_t *lower, anvil_block_t *src_block, anvil_block_t *dest_block)
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

static bool emit_phi_edge_block(ppc_mir_lower_t *lower, anvil_block_t *src_block, anvil_block_t *dest_block, anvil_mir_block_t edge_block, anvil_mir_block_t dest_mir_block)
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

static bool prepare_phi_aware_target(ppc_mir_lower_t *lower, anvil_block_t *src_block, anvil_block_t *dest_block, anvil_mir_block_t *out_target, pending_phi_edge_t **edges, size_t *num_edges,
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

static bool emit_pending_phi_edges(ppc_mir_lower_t *lower, anvil_block_t *src_block, pending_phi_edge_t *edges, size_t num_edges)
{
    for (size_t i = 0; i < num_edges; i++) {
        if (!emit_phi_edge_block(lower, src_block, edges[i].dest_block, edges[i].edge_block, edges[i].dest_mir_block)) {
            return false;
        }
    }
    return true;
}

static anvil_mir_block_t create_switch_chain_block(ppc_mir_lower_t *lower, anvil_block_t *src_block, size_t case_index)
{
    const char *src_name = (src_block && src_block->name) ? src_block->name : "anon";

    char name[256];
    snprintf(name, sizeof(name), "%s_switch_case_%zu_%zu", src_name, case_index, lower->num_edge_blocks++);
    return anvil_mir_add_block(lower->mir, name);
}

static bool lower_call(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands == 0)
        return false;
    if (instr->call_cc != ANVIL_CC_SYSV)
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
    size_t max_call_uses = num_args + (direct_call ? 0 : 1);
    anvil_mir_vreg_t *call_uses = NULL;
    if (max_call_uses > 0) {
        call_uses = calloc(max_call_uses, sizeof(*call_uses));
        if (!call_uses)
            return false;
    }

    size_t gpr_count = 0;
    size_t fpr_count = 0;
    size_t num_call_uses = 0;
    int64_t stack_offset = 0;
    bool ok = true;

    if (!direct_call) {
        anvil_mir_vreg_t target_src = lower_value(lower, callee);
        anvil_mir_vreg_t target_fixed = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->word_size * 8), false);
        if (target_src == ANVIL_MIR_NO_VREG || target_fixed == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->mir, target_fixed, lower->desc->indirect_call_reg)) {
            free(call_uses);
            return false;
        }

        anvil_mir_vreg_t target_uses[] = {target_src};
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, target_fixed, target_uses, 1)) {
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
            anvil_mir_vreg_t stack_use[] = {src};
            if (!anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, stack_use, 1, stack_offset)) {
                ok = false;
                break;
            }
            stack_offset += ppc_stack_arg_slot_size(lower, arg->type);
            advance_arg_count(arg->type, &gpr_count, &fpr_count);
            continue;
        }

        anvil_mir_vreg_t fixed_arg = ppc_add_vreg_for_type(lower, arg->type);
        if (fixed_arg == ANVIL_MIR_NO_VREG || !set_fixed_register_arg(lower, fixed_arg, arg->type, &gpr_count, &fpr_count)) {
            ok = false;
            break;
        }

        anvil_mir_vreg_t copy_uses[] = {src};
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, fixed_arg, copy_uses, 1)) {
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
        if (call_def == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->mir, call_def, ppc_type_is_fp(instr->result->type) ? lower->desc->fpr_return_reg : lower->desc->gpr_return_reg)) {
            free(call_uses);
            return false;
        }
    }

    ok = anvil_mir_add_call(lower->mir, call_def, call_uses, num_call_uses, symbol, instr->call_cc, false, 0);
    free(call_uses);
    if (!ok)
        return false;

    if (instr->result && call_def != ANVIL_MIR_NO_VREG) {
        anvil_mir_vreg_t local_result = ppc_add_vreg_for_type(lower, instr->result->type);
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

static anvil_mir_vreg_t lower_widen_gpr_to_word(ppc_mir_lower_t *lower, anvil_mir_vreg_t src, bool sign_extend)
{
    const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->mir, src);
    if (!src_info || src_info->reg_class != ANVIL_MIR_REG_GPR) {
        return ANVIL_MIR_NO_VREG;
    }

    uint16_t word_bits = (uint16_t)(lower->desc->word_size * 8);
    if (src_info->size_bits == word_bits)
        return src;

    if (src_info->size_bits > word_bits) {
        anvil_mir_vreg_t narrow = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, word_bits, sign_extend);
        if (narrow == ANVIL_MIR_NO_VREG)
            return ANVIL_MIR_NO_VREG;
        anvil_mir_vreg_t uses[] = {src};
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_TRUNC, narrow, uses, 1)) {
            return ANVIL_MIR_NO_VREG;
        }
        return narrow;
    }

    anvil_mir_vreg_t wide = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, word_bits, sign_extend);
    if (wide == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;

    anvil_mir_vreg_t uses[] = {src};
    anvil_mir_opcode_t op = sign_extend ? ANVIL_MIR_OP_SEXT : ANVIL_MIR_OP_ZEXT;
    if (!anvil_mir_add_instr(lower->mir, op, wide, uses, 1)) {
        return ANVIL_MIR_NO_VREG;
    }
    return wide;
}

static anvil_mir_vreg_t lower_resize_gpr(ppc_mir_lower_t *lower, anvil_mir_vreg_t src, uint16_t target_bits)
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

static bool lower_match_binary_operand_sizes(ppc_mir_lower_t *lower, anvil_mir_vreg_t *lhs, anvil_mir_vreg_t *rhs, anvil_mir_vreg_t def)
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

static bool lower_alloca(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || !instr->result->type || instr->result->type->kind != ANVIL_TYPE_PTR) {
        return false;
    }

    anvil_type_t *element_type = instr->aux_type;
    if (!element_type)
        element_type = instr->result->type->data.pointee;
    if (!element_type)
        return false;

    anvil_mir_vreg_t ptr = ppc_add_vreg_for_type(lower, instr->result->type);
    if (ptr == ANVIL_MIR_NO_VREG)
        return false;

    if (instr->num_operands == 0) {
        int slot = anvil_mir_add_frame_slot(lower->mir, ppc_slot_bits_for_type(lower->desc, element_type), ppc_align_for_type(lower->desc, element_type));
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
    count = lower_widen_gpr_to_word(lower, count, false);
    if (count == ANVIL_MIR_NO_VREG)
        return false;

    int64_t elem_size = element_type ? (int64_t)anvil_type_size(element_type) : 1;
    if (elem_size <= 0)
        elem_size = 1;
    anvil_mir_vreg_t uses[] = {count};
    if (!anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_DYN_ALLOCA, ptr, uses, 1, elem_size)) {
        return false;
    }
    return map_put(lower, instr->result, ptr);
}

static bool lower_add_const_offset(ppc_mir_lower_t *lower, anvil_mir_vreg_t base, int64_t offset, anvil_mir_vreg_t *out_ptr)
{
    if (offset == 0) {
        *out_ptr = base;
        return true;
    }

    anvil_mir_vreg_t off = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->word_size * 8));
    anvil_mir_vreg_t dst = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->word_size * 8));
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

static bool lower_gep(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands < 2 || !instr->result || !instr->aux_type)
        return false;

    anvil_mir_vreg_t current = lower_value(lower, instr->operands[0]);
    if (current == ANVIL_MIR_NO_VREG)
        return false;
    current = lower_widen_gpr_to_word(lower, current, false);
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
        if (ppc_needs_i64_pair(lower, index_value->type)) {
            anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
            if (!ppc_ensure_i64_pair(lower, index_value, &hi, &index, NULL))
                return false;
            /* PPC32 represents i64 values as {hi,lo}; the low word is the
             * required pointer-width truncation. */
        } else {
            index = lower_value(lower, index_value);
            if (index == ANVIL_MIR_NO_VREG)
                return false;
            index = lower_widen_gpr_to_word(lower, index, index_value->type->is_signed);
        }
        if (index == ANVIL_MIR_NO_VREG)
            return false;

        anvil_mir_vreg_t scaled = index;
        if (elem_size != 1) {
            anvil_mir_vreg_t scale = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->word_size * 8));
            scaled = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->word_size * 8));
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

        anvil_mir_vreg_t next = anvil_mir_add_vreg_ex(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->word_size * 8));
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

static bool lower_struct_gep(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands < 1 || !instr->result)
        return false;

    anvil_mir_vreg_t base = lower_value(lower, instr->operands[0]);
    if (base == ANVIL_MIR_NO_VREG)
        return false;
    base = lower_widen_gpr_to_word(lower, base, false);
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

static bool ppc_ensure_i64_pair(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *hi, anvil_mir_vreg_t *lo, bool *is_unsigned);

static bool ppc_lower_i64_pair_to_fp(ppc_mir_lower_t *lower, anvil_instr_t *instr, anvil_mir_opcode_t mir_op)
{
    anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
    if (!ppc_ensure_i64_pair(lower, instr->operands[0], &hi, &lo, NULL)) {
        return false;
    }

    bool is_unsigned = mir_op == ANVIL_MIR_OP_UITOFP;
    uint16_t dst_bits = ppc_bits_for_type(lower->desc, instr->result->type);
    anvil_mir_vreg_t arg_hi = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, !is_unsigned);
    anvil_mir_vreg_t arg_lo = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, false);
    anvil_mir_vreg_t call_result = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_FPR, dst_bits, false);
    anvil_mir_vreg_t local_result = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_FPR, dst_bits, false);
    if (arg_hi == ANVIL_MIR_NO_VREG || arg_lo == ANVIL_MIR_NO_VREG || call_result == ANVIL_MIR_NO_VREG || local_result == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->mir, arg_hi, 3) ||
        !anvil_mir_set_fixed_reg(lower->mir, arg_lo, 4) || !anvil_mir_set_fixed_reg(lower->mir, call_result, 1)) {
        return false;
    }
    anvil_mir_vreg_t hi_uses[] = {hi};
    anvil_mir_vreg_t lo_uses[] = {lo};
    anvil_mir_vreg_t call_uses[] = {arg_hi, arg_lo};
    anvil_mir_vreg_t result_uses[] = {call_result};
    const char *helper = is_unsigned ? (dst_bits == 32 ? "__floatundisf" : "__floatundidf") : (dst_bits == 32 ? "__floatdisf" : "__floatdidf");
    return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, arg_hi, hi_uses, 1) && anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, arg_lo, lo_uses, 1) &&
           anvil_mir_add_call(lower->mir, call_result, call_uses, 2, helper, ANVIL_CC_SYSV, false, 0) && anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, local_result, result_uses, 1) &&
           map_put(lower, instr->result, local_result);
}

static bool ppc_lower_fp_to_i64_pair(ppc_mir_lower_t *lower, anvil_instr_t *instr, anvil_mir_opcode_t mir_op)
{
    anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
    if (src == ANVIL_MIR_NO_VREG)
        return false;
    bool is_unsigned = mir_op == ANVIL_MIR_OP_FPTOUI;
    const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->mir, src);
    if (!src_info || src_info->reg_class != ANVIL_MIR_REG_FPR)
        return false;
    anvil_mir_vreg_t arg = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_FPR, src_info->size_bits, false);
    anvil_mir_vreg_t ret_hi = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, !is_unsigned);
    anvil_mir_vreg_t ret_lo = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, false);
    if (arg == ANVIL_MIR_NO_VREG || ret_hi == ANVIL_MIR_NO_VREG || ret_lo == ANVIL_MIR_NO_VREG || !anvil_mir_set_fixed_reg(lower->mir, arg, 1) || !anvil_mir_set_fixed_reg(lower->mir, ret_hi, 3) ||
        !anvil_mir_set_fixed_reg(lower->mir, ret_lo, 4)) {
        return false;
    }
    anvil_mir_vreg_t arg_copy_uses[] = {src};
    anvil_mir_vreg_t call_uses[] = {arg};
    const char *helper;
    if (is_unsigned) {
        helper = src_info->size_bits == 32 ? "__fixunssfdi" : "__fixunsdfdi";
    } else {
        helper = src_info->size_bits == 32 ? "__fixsfdi" : "__fixdfdi";
    }
    return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, arg, arg_copy_uses, 1) && anvil_mir_add_call(lower->mir, ret_hi, call_uses, 1, helper, ANVIL_CC_SYSV, false, 0) &&
           anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_CALL_RESULT, ret_lo, NULL, 0) && wide_pair_put(lower, instr->result, ret_hi, ret_lo, is_unsigned);
}

static bool lower_cast(ppc_mir_lower_t *lower, anvil_instr_t *instr, anvil_mir_opcode_t mir_op)
{
    if (instr->num_operands != 1 || !instr->result)
        return false;
    if (ppc_needs_i64_pair(lower, instr->operands[0]->type) && (mir_op == ANVIL_MIR_OP_SITOFP || mir_op == ANVIL_MIR_OP_UITOFP)) {
        return ppc_lower_i64_pair_to_fp(lower, instr, mir_op);
    }
    if (ppc_needs_i64_pair(lower, instr->result->type)) {
        if (mir_op == ANVIL_MIR_OP_FPTOSI || mir_op == ANVIL_MIR_OP_FPTOUI) {
            return ppc_lower_fp_to_i64_pair(lower, instr, mir_op);
        }
        if (mir_op == ANVIL_MIR_OP_BITCAST) {
            return ppc_lower_i64_bitcast_pair(lower, instr);
        }
        return false;
    }

    anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t def = ppc_add_vreg_for_type(lower, instr->result->type);
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

    if (mir_op == ANVIL_MIR_OP_BITCAST) {
        const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->mir, src);
        const anvil_mir_vreg_info_t *dst_info = anvil_mir_get_vreg_info(lower->mir, def);
        if (!src_info || !dst_info)
            return false;
        if (src_info->size_bits != dst_info->size_bits) {
            if (instr->op != ANVIL_OP_PTRTOINT && instr->op != ANVIL_OP_INTTOPTR)
                return false;
            mir_op = src_info->size_bits < dst_info->size_bits ? ANVIL_MIR_OP_ZEXT : ANVIL_MIR_OP_TRUNC;
        }
    }

    anvil_mir_vreg_t uses[] = {src};
    if (!anvil_mir_add_instr(lower->mir, mir_op, def, uses, 1))
        return false;
    return map_put(lower, instr->result, def);
}

static bool lower_select(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands != 3 || !instr->result)
        return false;
    anvil_mir_vreg_t cond = lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t then_v = lower_value(lower, instr->operands[1]);
    anvil_mir_vreg_t else_v = lower_value(lower, instr->operands[2]);
    anvil_mir_vreg_t def = ppc_add_vreg_for_type(lower, instr->result->type);
    if (cond == ANVIL_MIR_NO_VREG || then_v == ANVIL_MIR_NO_VREG || else_v == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) {
        return false;
    }

    anvil_mir_vreg_t uses[] = {cond, then_v, else_v};
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_SELECT, def, uses, 3)) {
        return false;
    }
    return map_put(lower, instr->result, def);
}

static bool lower_memory_address(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *out_base, int64_t *out_offset)
{
    anvil_mir_vreg_t base = ANVIL_MIR_NO_VREG;
    int64_t offset = 0;
    if (addr_map_get(lower, value, &base, &offset)) {
        base = lower_widen_gpr_to_word(lower, base, false);
        if (base == ANVIL_MIR_NO_VREG)
            return false;
        *out_base = base;
        *out_offset = offset;
        return true;
    }

    base = lower_value(lower, value);
    if (base == ANVIL_MIR_NO_VREG)
        return false;
    base = lower_widen_gpr_to_word(lower, base, false);
    if (base == ANVIL_MIR_NO_VREG)
        return false;

    *out_base = base;
    *out_offset = 0;
    return true;
}

static bool ppc_add_memory_instr(ppc_mir_lower_t *lower, anvil_mir_opcode_t op, anvil_mir_vreg_t def, anvil_mir_vreg_t *uses, size_t num_uses, int64_t offset)
{
    if (offset == 0) {
        return anvil_mir_add_instr(lower->mir, op, def, uses, num_uses);
    }
    return anvil_mir_add_instr_imm_uses(lower->mir, op, def, uses, num_uses, offset);
}

static bool ppc_get_wide_const_value(ppc_mir_lower_t *lower, anvil_value_t *value, int64_t *out)
{
    if (ppc_get_const_int(value, out))
        return true;
    return wide_const_get(lower, value, out);
}

static anvil_mir_vreg_t ppc_add_i32_vreg(ppc_mir_lower_t *lower, bool is_signed)
{
    return anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, is_signed);
}

static anvil_mir_vreg_t ppc_add_bool_vreg(ppc_mir_lower_t *lower)
{
    return anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
}

static bool ppc_lower_i64_const_pair(ppc_mir_lower_t *lower, anvil_value_t *value)
{
    anvil_mir_vreg_t existing_hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t existing_lo = ANVIL_MIR_NO_VREG;
    if (wide_pair_get(lower, value, &existing_hi, &existing_lo, NULL)) {
        return true;
    }

    int64_t imm = 0;
    if (!ppc_get_const_int(value, &imm))
        return false;

    uint64_t bits = (uint64_t)imm;
    int32_t hi_imm = (int32_t)(bits >> 32);
    int32_t lo_imm = (int32_t)(bits & 0xffffffffu);
    bool is_unsigned = value->type && value->type->kind == ANVIL_TYPE_U64;

    anvil_mir_vreg_t hi = ppc_add_i32_vreg(lower, !is_unsigned);
    anvil_mir_vreg_t lo = ppc_add_i32_vreg(lower, false);
    if (hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG)
        return false;

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, hi, (int64_t)hi_imm) || !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, lo, (int64_t)lo_imm)) {
        return false;
    }

    return wide_const_put(lower, value, imm) && wide_pair_put(lower, value, hi, lo, is_unsigned);
}

static bool ppc_ensure_i64_pair(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *hi, anvil_mir_vreg_t *lo, bool *is_unsigned)
{
    if (wide_pair_get(lower, value, hi, lo, is_unsigned))
        return true;
    if (value && value->kind == ANVIL_VAL_CONST_INT && ppc_lower_i64_const_pair(lower, value)) {
        return wide_pair_get(lower, value, hi, lo, is_unsigned);
    }
    return false;
}

static bool ppc_lower_load_i64_pair(ppc_mir_lower_t *lower, anvil_instr_t *instr)
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
    anvil_mir_vreg_t hi = ppc_add_i32_vreg(lower, !is_unsigned);
    anvil_mir_vreg_t lo = ppc_add_i32_vreg(lower, false);
    if (hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG)
        return false;

    anvil_mir_vreg_t hi_uses[] = {base};
    if (!ppc_add_memory_instr(lower, ANVIL_MIR_OP_LOAD, hi, hi_uses, 1, offset)) {
        return false;
    }

    anvil_mir_vreg_t lo_uses[] = {base};
    if (!ppc_add_memory_instr(lower, ANVIL_MIR_OP_LOAD, lo, lo_uses, 1, offset + 4)) {
        return false;
    }

    return wide_pair_put(lower, instr->result, hi, lo, is_unsigned);
}

static bool ppc_lower_store_i64_const(ppc_mir_lower_t *lower, int64_t imm, anvil_mir_vreg_t base, int64_t offset)
{
    if (offset > INT64_MAX - 4)
        return false;

    uint64_t bits = (uint64_t)imm;
    int32_t hi_imm = (int32_t)(bits >> 32);
    int32_t lo_imm = (int32_t)(bits & 0xffffffffu);

    anvil_mir_vreg_t hi = ppc_add_i32_vreg(lower, true);
    anvil_mir_vreg_t lo = ppc_add_i32_vreg(lower, false);
    if (hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG)
        return false;

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, hi, (int64_t)hi_imm)) {
        return false;
    }
    anvil_mir_vreg_t hi_uses[] = {hi, base};
    if (!ppc_add_memory_instr(lower, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, hi_uses, 2, offset)) {
        return false;
    }

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, lo, (int64_t)lo_imm)) {
        return false;
    }
    anvil_mir_vreg_t lo_uses[] = {lo, base};
    return ppc_add_memory_instr(lower, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, lo_uses, 2, offset + 4);
}

static bool ppc_lower_store_i64_pair(ppc_mir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t base, int64_t offset)
{
    if (offset > INT64_MAX - 4)
        return false;

    anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
    if (!ppc_ensure_i64_pair(lower, value, &hi, &lo, NULL))
        return false;

    anvil_mir_vreg_t hi_uses[] = {hi, base};
    if (!ppc_add_memory_instr(lower, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, hi_uses, 2, offset)) {
        return false;
    }

    anvil_mir_vreg_t lo_uses[] = {lo, base};
    return ppc_add_memory_instr(lower, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, lo_uses, 2, offset + 4);
}

static bool ppc_lower_i64_bitcast_pair(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || instr->num_operands != 1)
        return false;

    anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
    if (!ppc_ensure_i64_pair(lower, instr->operands[0], &hi, &lo, NULL)) {
        return false;
    }

    int64_t imm = 0;
    if (ppc_get_wide_const_value(lower, instr->operands[0], &imm) && !wide_const_put(lower, instr->result, imm)) {
        return false;
    }

    bool is_unsigned = instr->result->type && instr->result->type->kind == ANVIL_TYPE_U64;
    return wide_pair_put(lower, instr->result, hi, lo, is_unsigned);
}

static bool ppc_add_pair_cmp(ppc_mir_lower_t *lower, anvil_mir_opcode_t op, anvil_mir_vreg_t def, anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs)
{
    anvil_mir_vreg_t uses[] = {lhs, rhs};
    return anvil_mir_add_instr(lower->mir, op, def, uses, 2);
}

static bool ppc_lower_i64_cmp_pair(ppc_mir_lower_t *lower, anvil_instr_t *instr, anvil_mir_opcode_t op)
{
    if (!instr->result || instr->num_operands != 2)
        return false;

    anvil_mir_vreg_t lhi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t llo = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t rhi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t rlo = ANVIL_MIR_NO_VREG;
    if (!ppc_ensure_i64_pair(lower, instr->operands[0], &lhi, &llo, NULL) || !ppc_ensure_i64_pair(lower, instr->operands[1], &rhi, &rlo, NULL)) {
        return false;
    }

    anvil_mir_vreg_t result = ppc_add_bool_vreg(lower);
    if (result == ANVIL_MIR_NO_VREG)
        return false;

    if (op == ANVIL_MIR_OP_CMP_EQ || op == ANVIL_MIR_OP_CMP_NE) {
        anvil_mir_vreg_t hi_cmp = ppc_add_bool_vreg(lower);
        anvil_mir_vreg_t lo_cmp = ppc_add_bool_vreg(lower);
        if (hi_cmp == ANVIL_MIR_NO_VREG || lo_cmp == ANVIL_MIR_NO_VREG) {
            return false;
        }
        if (!ppc_add_pair_cmp(lower, op, hi_cmp, lhi, rhi) || !ppc_add_pair_cmp(lower, op, lo_cmp, llo, rlo)) {
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

    anvil_mir_vreg_t hi_cmp = ppc_add_bool_vreg(lower);
    anvil_mir_vreg_t hi_eq = ppc_add_bool_vreg(lower);
    anvil_mir_vreg_t lo_cmp = ppc_add_bool_vreg(lower);
    anvil_mir_vreg_t eq_and_lo = ppc_add_bool_vreg(lower);
    if (hi_cmp == ANVIL_MIR_NO_VREG || hi_eq == ANVIL_MIR_NO_VREG || lo_cmp == ANVIL_MIR_NO_VREG || eq_and_lo == ANVIL_MIR_NO_VREG) {
        return false;
    }

    if (!ppc_add_pair_cmp(lower, hi_rel, hi_cmp, lhi, rhi) || !ppc_add_pair_cmp(lower, ANVIL_MIR_OP_CMP_EQ, hi_eq, lhi, rhi) || !ppc_add_pair_cmp(lower, lo_rel, lo_cmp, llo, rlo)) {
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

static bool lower_switch(ppc_mir_lower_t *lower, anvil_instr_t *instr)
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

    if (ok)
        ok = emit_pending_phi_edges(lower, instr->parent, edges, num_edges);
    free(edges);
    return ok;
}

static bool lower_instr(ppc_mir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->op == ANVIL_OP_NOP)
        return true;
    if (instr->op == ANVIL_OP_PHI)
        return true;

    anvil_mir_opcode_t mir_op;
    if (instr->num_operands == 2 && map_binop(instr->op, &mir_op)) {
        if (ppc_needs_i64_pair(lower, instr->operands[0]->type) && ppc_needs_i64_pair(lower, instr->operands[1]->type) && mir_op_is_compare(mir_op)) {
            return ppc_lower_i64_cmp_pair(lower, instr, mir_op);
        }

        anvil_mir_vreg_t lhs = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t rhs = lower_value(lower, instr->operands[1]);
        anvil_mir_vreg_t def = instr->result ? ppc_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
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
        if (instr->result && ppc_needs_i64_pair(lower, instr->result->type)) {
            int64_t imm = 0;
            if (!ppc_get_wide_const_value(lower, instr->operands[0], &imm)) {
                return false;
            }
            if (instr->op == ANVIL_OP_NEG) {
                return wide_const_put(lower, instr->result, (int64_t)(0 - (uint64_t)imm));
            }
            if (instr->op == ANVIL_OP_NOT) {
                return wide_const_put(lower, instr->result, ~imm);
            }
            return false;
        }

        anvil_mir_vreg_t src = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t def = instr->result ? ppc_add_vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
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
        if (ppc_needs_i64_pair(lower, instr->result->type)) {
            return ppc_lower_load_i64_pair(lower, instr);
        }

        anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
        int64_t offset = 0;
        if (!lower_memory_address(lower, instr->operands[0], &ptr, &offset)) {
            return false;
        }
        anvil_mir_vreg_t def = ppc_add_vreg_for_type(lower, instr->result->type);
        if (def == ANVIL_MIR_NO_VREG)
            return false;
        bool is_i1 = instr->result->type->kind == ANVIL_TYPE_I1;
        anvil_mir_vreg_t loaded = is_i1 ? anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false) : def;
        if (loaded == ANVIL_MIR_NO_VREG)
            return false;
        anvil_mir_vreg_t uses[] = {ptr};
        bool ok = offset == 0 ? anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_LOAD, loaded, uses, 1) : anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_LOAD, loaded, uses, 1, offset);
        if (!ok)
            return false;
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
        if (ppc_needs_i64_pair(lower, instr->operands[0]->type)) {
            anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
            int64_t offset = 0;
            if (!lower_memory_address(lower, instr->operands[1], &ptr, &offset)) {
                return false;
            }

            int64_t imm = 0;
            if (ppc_get_wide_const_value(lower, instr->operands[0], &imm)) {
                return ppc_lower_store_i64_const(lower, imm, ptr, offset);
            }
            if (wide_pair_get(lower, instr->operands[0], NULL, NULL, NULL)) {
                return ppc_lower_store_i64_pair(lower, instr->operands[0], ptr, offset);
            }
        }

        anvil_mir_vreg_t val = lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t ptr = ANVIL_MIR_NO_VREG;
        int64_t offset = 0;
        if (!lower_memory_address(lower, instr->operands[1], &ptr, &offset)) {
            return false;
        }
        if (val == ANVIL_MIR_NO_VREG || ptr == ANVIL_MIR_NO_VREG) {
            return false;
        }
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
            return add_return_copy(lower, NULL);
        if (instr->num_operands == 1) {
            return add_return_copy(lower, instr->operands[0]);
        }
        return false;
    default:
        return false;
    }
}

anvil_mir_func_t *anvil_ppc_lower_func_to_mir(anvil_func_t *func, anvil_ppc_variant_t variant)
{
    const anvil_ppc_target_desc_t *desc = anvil_ppc_get_target_desc(variant);
    if (!desc || !func || func->is_declaration || !func->type || func->type->kind != ANVIL_TYPE_FUNC || func->type->data.func.cc != ANVIL_CC_SYSV)
        return NULL;

    ppc_mir_lower_t lower;
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
        ppc_lower_free(&lower);
        return NULL;
    }

    for (size_t rank = 0; rank < cfg.count; rank++) {
        anvil_block_t *block = cfg.blocks[cfg.rpo[rank]];
        anvil_mir_block_t mir_block = block_get(&lower, block);
        if (mir_block == ANVIL_MIR_NO_BLOCK || !anvil_mir_set_current_block(lower.mir, mir_block)) {
            anvil_opt_cfg_destroy(&cfg);
            anvil_mir_func_destroy(lower.mir);
            ppc_lower_free(&lower);
            return NULL;
        }

        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            size_t first = anvil_mir_num_instrs(lower.mir);
            if (!lower_instr(&lower, instr) || !anvil_mir_annotate_memory(lower.mir, first, &instr->memory_access) ||
                (instr->op == ANVIL_OP_CALL && !anvil_mir_annotate_call_effects(lower.mir, first, anvil_instr_call_effects(instr)))) {
                anvil_opt_cfg_destroy(&cfg);
                anvil_mir_func_destroy(lower.mir);
                ppc_lower_free(&lower);
                return NULL;
            }
        }
    }

    ppc_lower_free(&lower);
    anvil_opt_cfg_destroy(&cfg);
    return lower.mir;
}
