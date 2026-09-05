#include "systemz_internal.h"

typedef struct {
    anvil_value_t *value;
    anvil_mir_vreg_t vreg;
} systemz_value_map_entry_t;

typedef struct {
    anvil_block_t *block;
    anvil_mir_block_t mir_block;
} systemz_block_map_entry_t;

typedef struct {
    anvil_value_t *value;
    int frame_slot;
} systemz_addr_map_entry_t;

typedef struct {
    anvil_value_t *value;
    int64_t imm;
} systemz_wide_const_map_entry_t;

typedef struct {
    anvil_value_t *value;
    anvil_mir_vreg_t hi;
    anvil_mir_vreg_t lo;
    bool is_unsigned;
} systemz_wide_pair_map_entry_t;

typedef struct {
    const anvil_mainframe_target_desc_t *desc;
    anvil_func_t *func;
    anvil_mir_func_t *mir;

    systemz_value_map_entry_t *values;
    size_t num_values;
    size_t cap_values;

    systemz_block_map_entry_t *blocks;
    size_t num_blocks;

    systemz_addr_map_entry_t *frame_addrs;
    size_t num_frame_addrs;
    size_t cap_frame_addrs;

    systemz_wide_const_map_entry_t *wide_consts;
    size_t num_wide_consts;
    size_t cap_wide_consts;

    systemz_wide_pair_map_entry_t *wide_pairs;
    size_t num_wide_pairs;
    size_t cap_wide_pairs;
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

static uint16_t systemz_slot_bits_for_type(const anvil_mainframe_target_desc_t *desc, anvil_type_t *type)
{
    uint16_t bits = systemz_bits_for_type(desc, type);
    uint16_t word_bits = (uint16_t)(desc->word_size * 8);
    return bits < word_bits ? word_bits : bits;
}

static uint16_t systemz_align_for_type(const anvil_mainframe_target_desc_t *desc, anvil_type_t *type)
{
    if (!type)
        return (uint16_t)desc->word_size;
    if (type->align == 0)
        return 1;
    if (type->align > desc->word_size)
        return (uint16_t)desc->word_size;
    return (uint16_t)type->align;
}

static anvil_mir_vreg_t systemz_add_vreg_for_type(systemz_lower_t *lower, anvil_type_t *type)
{
    return anvil_mir_add_vreg_typed(lower->mir, systemz_reg_class_for_type(type), systemz_bits_for_type(lower->desc, type), systemz_type_is_signed(type));
}

static bool systemz_map_reserve(systemz_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_values)
        return true;
    size_t cap = lower->cap_values ? lower->cap_values * 2 : 64;
    while (cap < needed)
        cap *= 2;
    systemz_value_map_entry_t *grown = realloc(lower->values, cap * sizeof(*grown));
    if (!grown)
        return false;
    lower->values = grown;
    lower->cap_values = cap;
    return true;
}

static bool systemz_map_put(systemz_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t vreg)
{
    if (!value)
        return true;
    for (size_t i = 0; i < lower->num_values; i++) {
        if (lower->values[i].value == value) {
            lower->values[i].vreg = vreg;
            return true;
        }
    }
    if (!systemz_map_reserve(lower, lower->num_values + 1))
        return false;
    lower->values[lower->num_values].value = value;
    lower->values[lower->num_values].vreg = vreg;
    lower->num_values++;
    return true;
}

static anvil_mir_vreg_t systemz_map_get(systemz_lower_t *lower, anvil_value_t *value)
{
    for (size_t i = 0; i < lower->num_values; i++) {
        if (lower->values[i].value == value)
            return lower->values[i].vreg;
    }
    return ANVIL_MIR_NO_VREG;
}

static bool systemz_addr_map_reserve(systemz_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_frame_addrs)
        return true;
    size_t cap = lower->cap_frame_addrs ? lower->cap_frame_addrs * 2 : 16;
    while (cap < needed)
        cap *= 2;
    systemz_addr_map_entry_t *grown = realloc(lower->frame_addrs, cap * sizeof(*grown));
    if (!grown)
        return false;
    lower->frame_addrs = grown;
    lower->cap_frame_addrs = cap;
    return true;
}

static bool systemz_addr_map_put(systemz_lower_t *lower, anvil_value_t *value, int frame_slot)
{
    if (!value)
        return false;
    for (size_t i = 0; i < lower->num_frame_addrs; i++) {
        if (lower->frame_addrs[i].value == value) {
            lower->frame_addrs[i].frame_slot = frame_slot;
            return true;
        }
    }
    if (!systemz_addr_map_reserve(lower, lower->num_frame_addrs + 1))
        return false;
    lower->frame_addrs[lower->num_frame_addrs].value = value;
    lower->frame_addrs[lower->num_frame_addrs].frame_slot = frame_slot;
    lower->num_frame_addrs++;
    return true;
}

static anvil_mir_block_t systemz_block_get(systemz_lower_t *lower, anvil_block_t *block)
{
    if (!block)
        return ANVIL_MIR_NO_BLOCK;
    for (size_t i = 0; i < lower->num_blocks; i++) {
        if (lower->blocks[i].block == block)
            return lower->blocks[i].mir_block;
    }
    return ANVIL_MIR_NO_BLOCK;
}

static bool systemz_wide_const_reserve(systemz_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_wide_consts)
        return true;
    size_t cap = lower->cap_wide_consts ? lower->cap_wide_consts * 2 : 8;
    while (cap < needed)
        cap *= 2;
    systemz_wide_const_map_entry_t *grown = realloc(lower->wide_consts, cap * sizeof(*grown));
    if (!grown)
        return false;
    lower->wide_consts = grown;
    lower->cap_wide_consts = cap;
    return true;
}

static bool systemz_wide_const_put(systemz_lower_t *lower, anvil_value_t *value, int64_t imm)
{
    if (!value)
        return false;
    for (size_t i = 0; i < lower->num_wide_consts; i++) {
        if (lower->wide_consts[i].value == value) {
            lower->wide_consts[i].imm = imm;
            return true;
        }
    }
    if (!systemz_wide_const_reserve(lower, lower->num_wide_consts + 1))
        return false;
    lower->wide_consts[lower->num_wide_consts].value = value;
    lower->wide_consts[lower->num_wide_consts].imm = imm;
    lower->num_wide_consts++;
    return true;
}

static bool systemz_wide_const_get(systemz_lower_t *lower, anvil_value_t *value, int64_t *out)
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

static bool systemz_wide_pair_reserve(systemz_lower_t *lower, size_t needed)
{
    if (needed <= lower->cap_wide_pairs)
        return true;
    size_t cap = lower->cap_wide_pairs ? lower->cap_wide_pairs * 2 : 8;
    while (cap < needed)
        cap *= 2;
    systemz_wide_pair_map_entry_t *grown = realloc(lower->wide_pairs, cap * sizeof(*grown));
    if (!grown)
        return false;
    lower->wide_pairs = grown;
    lower->cap_wide_pairs = cap;
    return true;
}

static bool systemz_wide_pair_put(systemz_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t hi, anvil_mir_vreg_t lo, bool is_unsigned)
{
    if (!value || hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG)
        return false;
    for (size_t i = 0; i < lower->num_wide_pairs; i++) {
        if (lower->wide_pairs[i].value == value) {
            lower->wide_pairs[i].hi = hi;
            lower->wide_pairs[i].lo = lo;
            lower->wide_pairs[i].is_unsigned = is_unsigned;
            return true;
        }
    }
    if (!systemz_wide_pair_reserve(lower, lower->num_wide_pairs + 1))
        return false;
    lower->wide_pairs[lower->num_wide_pairs].value = value;
    lower->wide_pairs[lower->num_wide_pairs].hi = hi;
    lower->wide_pairs[lower->num_wide_pairs].lo = lo;
    lower->wide_pairs[lower->num_wide_pairs].is_unsigned = is_unsigned;
    lower->num_wide_pairs++;
    return true;
}

static bool systemz_wide_pair_get(systemz_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t *hi, anvil_mir_vreg_t *lo, bool *is_unsigned)
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

static bool systemz_create_mir_blocks(systemz_lower_t *lower)
{
    lower->num_blocks = lower->func->num_blocks;
    if (lower->num_blocks == 0)
        return false;
    lower->blocks = calloc(lower->num_blocks, sizeof(*lower->blocks));
    if (!lower->blocks)
        return false;

    size_t idx = 0;
    for (anvil_block_t *block = lower->func->blocks; block; block = block->next) {
        if (idx >= lower->num_blocks)
            return false;
        anvil_mir_block_t mir_block = idx == 0 ? anvil_mir_current_block(lower->mir) : anvil_mir_add_block(lower->mir, block->name);
        if (mir_block == ANVIL_MIR_NO_BLOCK)
            return false;
        lower->blocks[idx].block = block;
        lower->blocks[idx].mir_block = mir_block;
        idx++;
    }
    return idx == lower->num_blocks;
}

static bool systemz_lower_params(systemz_lower_t *lower)
{
    for (size_t i = 0; i < lower->func->num_params; i++) {
        anvil_value_t *param = lower->func->params[i];
        anvil_mir_vreg_t local = systemz_add_vreg_for_type(lower, param->type);
        if (local == ANVIL_MIR_NO_VREG)
            return false;
        int64_t slot = (int64_t)i * (int64_t)lower->desc->ptr_size;
        if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, local, slot)) {
            return false;
        }
        if (!systemz_map_put(lower, param, local))
            return false;
    }
    return true;
}

static bool systemz_prepare_phi_results(systemz_lower_t *lower)
{
    for (anvil_block_t *block = lower->func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_PHI)
                continue;
            if (!instr->result)
                continue;
            anvil_mir_vreg_t vreg = systemz_add_vreg_for_type(lower, instr->result->type);
            if (vreg == ANVIL_MIR_NO_VREG)
                return false;
            if (!systemz_map_put(lower, instr->result, vreg))
                return false;
        }
    }
    return true;
}

static anvil_mir_vreg_t systemz_lower_value(systemz_lower_t *lower, anvil_value_t *value);

static int64_t systemz_int_constant(anvil_value_t *value)
{
    if (!value)
        return 0;
    if (value->type && !value->type->is_signed)
        return (int64_t)value->data.u;
    return value->data.i;
}

static bool systemz_get_const_int(anvil_value_t *value, int64_t *out)
{
    if (!value || value->kind != ANVIL_VAL_CONST_INT)
        return false;
    if (out)
        *out = systemz_int_constant(value);
    return true;
}

static anvil_mir_vreg_t systemz_lower_const_value(systemz_lower_t *lower, anvil_value_t *value)
{
    anvil_mir_vreg_t existing = systemz_map_get(lower, value);
    if (existing != ANVIL_MIR_NO_VREG)
        return existing;

    anvil_mir_vreg_t vreg = systemz_add_vreg_for_type(lower, value->type);
    if (vreg == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;

    int64_t imm = 0;
    if (value->kind == ANVIL_VAL_CONST_NULL) {
        imm = 0;
    } else if (value->kind == ANVIL_VAL_CONST_FLOAT) {
        if (value->type && value->type->kind == ANVIL_TYPE_F32) {
            union {
                float f;
                uint32_t u;
            } cvt;
            cvt.f = (float)value->data.f;
            imm = (int64_t)cvt.u;
        } else {
            union {
                double d;
                uint64_t u;
            } cvt;
            cvt.d = value->data.f;
            imm = (int64_t)cvt.u;
        }
    } else {
        imm = systemz_int_constant(value);
    }

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, vreg, imm)) {
        return ANVIL_MIR_NO_VREG;
    }
    if (!systemz_map_put(lower, value, vreg))
        return ANVIL_MIR_NO_VREG;
    return vreg;
}

static anvil_mir_vreg_t systemz_lower_symbol_address(systemz_lower_t *lower, anvil_type_t *type, const char *symbol)
{
    (void)type;
    anvil_mir_vreg_t vreg = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->ptr_size * 8), false);
    if (vreg == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;

    if (!anvil_mir_add_instr_symbol(lower->mir, ANVIL_MIR_OP_SYMBOL_ADDR, vreg, NULL, 0, symbol)) {
        return ANVIL_MIR_NO_VREG;
    }
    return vreg;
}

static anvil_mir_vreg_t systemz_lower_reloc_address(systemz_lower_t *lower, anvil_value_t *value)
{
    const char *symbol = value && value->data.reloc.symbol ? value->data.reloc.symbol->name : NULL;
    if (!symbol || !symbol[0])
        return ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t vreg = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->ptr_size * 8), false);
    if (vreg == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_symbol_imm(lower->mir, ANVIL_MIR_OP_SYMBOL_ADDR, vreg, NULL, 0, symbol, value->data.reloc.addend))
        return ANVIL_MIR_NO_VREG;
    return vreg;
}

static anvil_mir_vreg_t systemz_lower_value(systemz_lower_t *lower, anvil_value_t *value)
{
    if (!value)
        return ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t existing = systemz_map_get(lower, value);
    if (existing != ANVIL_MIR_NO_VREG)
        return existing;

    switch (value->kind) {
    case ANVIL_VAL_CONST_INT:
    case ANVIL_VAL_CONST_FLOAT:
    case ANVIL_VAL_CONST_NULL:
        return systemz_lower_const_value(lower, value);

    case ANVIL_VAL_CONST_STRING: {
        const char *label = NULL;
        if (anvil_mir_add_string_literal(lower->mir, value->data.str ? value->data.str : "", &label) < 0) {
            return ANVIL_MIR_NO_VREG;
        }
        anvil_mir_vreg_t vreg = systemz_lower_symbol_address(lower, value->type, label);
        if (vreg == ANVIL_MIR_NO_VREG)
            return vreg;
        if (!systemz_map_put(lower, value, vreg))
            return ANVIL_MIR_NO_VREG;
        return vreg;
    }

    case ANVIL_VAL_CONST_SYMBOL_ADDR:
    case ANVIL_VAL_CONST_GEP: {
        anvil_mir_vreg_t vreg = systemz_lower_reloc_address(lower, value);
        if (vreg == ANVIL_MIR_NO_VREG || !systemz_map_put(lower, value, vreg))
            return ANVIL_MIR_NO_VREG;
        return vreg;
    }

    case ANVIL_VAL_GLOBAL:
    case ANVIL_VAL_FUNC: {
        anvil_mir_vreg_t vreg = systemz_lower_symbol_address(lower, value->type, value->name);
        if (vreg == ANVIL_MIR_NO_VREG)
            return vreg;
        if (!systemz_map_put(lower, value, vreg))
            return ANVIL_MIR_NO_VREG;
        return vreg;
    }

    case ANVIL_VAL_PARAM:
    case ANVIL_VAL_INSTR:
        return existing;

    case ANVIL_VAL_CONST_DECIMAL:
    case ANVIL_VAL_CONST_ARRAY:
    case ANVIL_VAL_CONST_STRUCT:
    case ANVIL_VAL_BLOCK:
        return ANVIL_MIR_NO_VREG;
    }

    return ANVIL_MIR_NO_VREG;
}

static bool systemz_binary_op(anvil_op_t op, anvil_mir_opcode_t *out_op)
{
    switch (op) {
    case ANVIL_OP_ADD:
        *out_op = ANVIL_MIR_OP_ADD;
        return true;
    case ANVIL_OP_SUB:
        *out_op = ANVIL_MIR_OP_SUB;
        return true;
    case ANVIL_OP_MUL:
        *out_op = ANVIL_MIR_OP_MUL;
        return true;
    case ANVIL_OP_SDIV:
        *out_op = ANVIL_MIR_OP_SDIV;
        return true;
    case ANVIL_OP_UDIV:
        *out_op = ANVIL_MIR_OP_UDIV;
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
    case ANVIL_OP_FADD:
        *out_op = ANVIL_MIR_OP_ADD;
        return true;
    case ANVIL_OP_FSUB:
        *out_op = ANVIL_MIR_OP_SUB;
        return true;
    case ANVIL_OP_FMUL:
        *out_op = ANVIL_MIR_OP_MUL;
        return true;
    case ANVIL_OP_FDIV:
        *out_op = ANVIL_MIR_OP_FDIV;
        return true;
    case ANVIL_OP_FCMP:
        *out_op = ANVIL_MIR_OP_FCMP;
        return true;
    default:
        return false;
    }
}

static bool systemz_unary_op(anvil_op_t op, anvil_mir_opcode_t *out_op)
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

static bool systemz_value_needs_materialization(anvil_value_t *value)
{
    if (!value)
        return false;
    switch (value->kind) {
    case ANVIL_VAL_CONST_INT:
    case ANVIL_VAL_CONST_FLOAT:
    case ANVIL_VAL_CONST_NULL:
    case ANVIL_VAL_CONST_STRING:
    case ANVIL_VAL_CONST_SYMBOL_ADDR:
    case ANVIL_VAL_CONST_GEP:
    case ANVIL_VAL_GLOBAL:
    case ANVIL_VAL_FUNC:
        return true;

    case ANVIL_VAL_CONST_DECIMAL:
    case ANVIL_VAL_CONST_ARRAY:
    case ANVIL_VAL_CONST_STRUCT:
    case ANVIL_VAL_PARAM:
    case ANVIL_VAL_INSTR:
    case ANVIL_VAL_BLOCK:
        return false;
    }
    return false;
}

/* Return true exactly for operands which systemz_lower_instr lowers through
   systemz_lower_value.  Materializable values are shared by the source IR, so
   lowering one lazily in a branch (or in a synthetic PHI edge block) and
   caching its vreg would make later uses reuse a definition which does not
   dominate them.  We pre-materialize those values in the MIR entry block.

   This predicate intentionally mirrors the exceptional lowering paths: GEP
   constant indices and struct field numbers are folded, direct callees are
   carried by CALL's symbol, and 64-bit constants on narrow mainframes are
   consumed by the split-word lowering rather than a 64-bit MIR vreg. */
static bool systemz_instr_lowers_operand(systemz_lower_t *lower, anvil_instr_t *instr, size_t operand_index)
{
    if (!lower || !instr || operand_index >= instr->num_operands)
        return false;

    anvil_mir_opcode_t ignored;
    if (systemz_binary_op(instr->op, &ignored)) {
        bool split_wide_compare = ignored == ANVIL_MIR_OP_CMP_EQ || ignored == ANVIL_MIR_OP_CMP_NE || ignored == ANVIL_MIR_OP_CMP_LT || ignored == ANVIL_MIR_OP_CMP_LE ||
                                  ignored == ANVIL_MIR_OP_CMP_GT || ignored == ANVIL_MIR_OP_CMP_GE || ignored == ANVIL_MIR_OP_CMP_ULT || ignored == ANVIL_MIR_OP_CMP_ULE ||
                                  ignored == ANVIL_MIR_OP_CMP_UGT || ignored == ANVIL_MIR_OP_CMP_UGE;
        if (!lower->desc->has_64bit_gprs && split_wide_compare && operand_index < 2u && systemz_type_is_64bit_integer(instr->operands[operand_index]->type)) {
            return false;
        }
        return operand_index < 2u;
    }
    if (systemz_unary_op(instr->op, &ignored)) {
        if (!lower->desc->has_64bit_gprs && instr->result && systemz_type_is_64bit_integer(instr->result->type)) {
            return false;
        }
        return operand_index == 0u;
    }

    switch (instr->op) {
    case ANVIL_OP_PHI:
        return true;
    case ANVIL_OP_ALLOCA:
        return instr->num_operands != 0u && operand_index == 0u;
    case ANVIL_OP_GEP:
        return operand_index == 0u || instr->operands[operand_index]->kind != ANVIL_VAL_CONST_INT;
    case ANVIL_OP_STRUCT_GEP:
    case ANVIL_OP_LOAD:
    case ANVIL_OP_BR_COND:
        return operand_index == 0u;
    case ANVIL_OP_STORE:
        if (operand_index == 0u && !lower->desc->has_64bit_gprs && systemz_type_is_64bit_integer(instr->operands[0]->type)) {
            return false;
        }
        return operand_index < 2u;
    case ANVIL_OP_CALL:
        return operand_index != 0u || instr->operands[0]->kind != ANVIL_VAL_FUNC;
    case ANVIL_OP_RET:
        return operand_index == 0u;
    case ANVIL_OP_SWITCH:
        return true;
    case ANVIL_OP_SELECT:
        return operand_index < 3u;

    case ANVIL_OP_NOP:
    case ANVIL_OP_BR:
        return false;
    default:
        return false;
    }
}

static bool systemz_materialize_entry_values(systemz_lower_t *lower)
{
    if (!lower)
        return false;
    for (anvil_block_t *block = lower->func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            for (size_t index = 0u; index < instr->num_operands; index++) {
                anvil_value_t *value = instr->operands[index];
                if (!systemz_instr_lowers_operand(lower, instr, index) || !systemz_value_needs_materialization(value)) {
                    continue;
                }
                if (systemz_lower_value(lower, value) == ANVIL_MIR_NO_VREG)
                    return false;
            }
        }
    }
    return true;
}

static bool systemz_add_return_copy(systemz_lower_t *lower, anvil_value_t *value)
{
    if (!value) {
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, NULL, 0);
    }

    anvil_mir_vreg_t src = systemz_lower_value(lower, value);
    if (src == ANVIL_MIR_NO_VREG)
        return false;

    anvil_mir_vreg_t ret = systemz_add_vreg_for_type(lower, value->type);
    if (ret == ANVIL_MIR_NO_VREG)
        return false;
    if (!anvil_mir_set_fixed_reg(lower->mir, ret, systemz_type_is_fp(value->type) ? lower->desc->return_fpr : lower->desc->return_gpr)) {
        return false;
    }

    anvil_mir_vreg_t uses[] = {src};
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, ret, uses, 1)) {
        return false;
    }
    uses[0] = ret;
    return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, uses, 1);
}

static bool systemz_emit_phi_copies_for_edge(systemz_lower_t *lower, anvil_block_t *pred, anvil_block_t *succ)
{
    if (!succ)
        return true;
    typedef anvil_mir_parallel_copy_t systemz_phi_copy_t;
    systemz_phi_copy_t *copies = NULL;
    size_t count = 0;
    size_t capacity = 0;
    for (anvil_instr_t *phi = succ->first; phi; phi = phi->next) {
        if (phi->op != ANVIL_OP_PHI)
            break;
        anvil_value_t *incoming = NULL;
        for (size_t i = 0; i < phi->num_phi_incoming; i++) {
            if (phi->phi_blocks && phi->phi_blocks[i] == pred) {
                incoming = phi->operands[i];
                break;
            }
        }
        if (!incoming || !phi->result)
            continue;
        anvil_mir_vreg_t src = systemz_lower_value(lower, incoming);
        anvil_mir_vreg_t dst = systemz_map_get(lower, phi->result);
        if (src == ANVIL_MIR_NO_VREG || dst == ANVIL_MIR_NO_VREG) {
            free(copies);
            return false;
        }
        if (src == dst)
            continue;
        if (count == capacity) {
            size_t grown_capacity = capacity ? capacity * 2 : 4;
            systemz_phi_copy_t *grown = realloc(copies, grown_capacity * sizeof(*grown));
            if (!grown) {
                free(copies);
                return false;
            }
            copies = grown;
            capacity = grown_capacity;
        }
        copies[count++] = (systemz_phi_copy_t){dst, src};
    }

    bool ok = anvil_mir_emit_parallel_copies(lower->mir, copies, count);
    free(copies);
    return ok;
}

static bool systemz_block_has_phi(anvil_block_t *block)
{
    return block && block->first && block->first->op == ANVIL_OP_PHI;
}

static anvil_mir_block_t systemz_create_phi_edge_block(systemz_lower_t *lower, anvil_block_t *pred, anvil_block_t *succ)
{
    if (!systemz_block_has_phi(succ))
        return systemz_block_get(lower, succ);

    char name[160];
    snprintf(name, sizeof(name), "%s_to_%s_phi_%zu", pred && pred->name ? pred->name : "pred", succ && succ->name ? succ->name : "succ", anvil_mir_num_blocks(lower->mir));
    anvil_mir_block_t previous = anvil_mir_current_block(lower->mir);
    anvil_mir_block_t edge = anvil_mir_add_block(lower->mir, name);
    if (edge == ANVIL_MIR_NO_BLOCK)
        return edge;

    if (!anvil_mir_set_current_block(lower->mir, edge))
        return ANVIL_MIR_NO_BLOCK;
    if (!systemz_emit_phi_copies_for_edge(lower, pred, succ))
        return ANVIL_MIR_NO_BLOCK;
    if (!anvil_mir_add_branch(lower->mir, systemz_block_get(lower, succ))) {
        return ANVIL_MIR_NO_BLOCK;
    }
    if (!anvil_mir_set_current_block(lower->mir, previous)) {
        return ANVIL_MIR_NO_BLOCK;
    }
    return edge;
}

static bool systemz_lower_alloca(systemz_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || !instr->result->type || instr->result->type->kind != ANVIL_TYPE_PTR) {
        return false;
    }

    anvil_type_t *elem = instr->aux_type ? instr->aux_type : instr->result->type->data.pointee;
    if (instr->num_operands == 0) {
        int slot = anvil_mir_add_frame_slot(lower->mir, systemz_slot_bits_for_type(lower->desc, elem), systemz_align_for_type(lower->desc, elem));
        if (slot < 0)
            return false;
        anvil_mir_vreg_t ptr = systemz_add_vreg_for_type(lower, instr->result->type);
        if (ptr == ANVIL_MIR_NO_VREG)
            return false;
        if (!anvil_mir_add_frame_addr(lower->mir, ptr, slot))
            return false;
        if (!systemz_addr_map_put(lower, instr->result, slot))
            return false;
        return systemz_map_put(lower, instr->result, ptr);
    }

    anvil_mir_vreg_t count = systemz_lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t ptr = systemz_add_vreg_for_type(lower, instr->result->type);
    if (count == ANVIL_MIR_NO_VREG || ptr == ANVIL_MIR_NO_VREG)
        return false;
    int64_t elem_size = elem && elem->size ? (int64_t)elem->size : 1;
    anvil_mir_vreg_t uses[] = {count};
    if (!anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_DYN_ALLOCA, ptr, uses, 1, elem_size)) {
        return false;
    }
    return systemz_map_put(lower, instr->result, ptr);
}

static bool systemz_lower_add_const_offset(systemz_lower_t *lower, anvil_mir_vreg_t base, int64_t offset, anvil_mir_vreg_t dst)
{
    if (offset == 0) {
        anvil_mir_vreg_t uses[] = {base};
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, dst, uses, 1);
    }

    anvil_mir_vreg_t off = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->ptr_size * 8), true);
    if (off == ANVIL_MIR_NO_VREG)
        return false;
    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, off, offset)) {
        return false;
    }
    anvil_mir_vreg_t uses[] = {base, off};
    return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_ADD, dst, uses, 2);
}

static anvil_mir_vreg_t systemz_normalize_gep_index(systemz_lower_t *lower, anvil_mir_vreg_t index, anvil_type_t *index_type)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(lower->mir, index);
    uint16_t ptr_bits = (uint16_t)(lower->desc->ptr_size * 8);
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR)
        return ANVIL_MIR_NO_VREG;
    if (info->size_bits == ptr_bits)
        return index;
    bool is_signed = index_type && index_type->is_signed;
    anvil_mir_vreg_t normalized = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, ptr_bits, is_signed);
    if (normalized == ANVIL_MIR_NO_VREG)
        return normalized;
    anvil_mir_opcode_t op = info->size_bits > ptr_bits ? ANVIL_MIR_OP_TRUNC : (is_signed ? ANVIL_MIR_OP_SEXT : ANVIL_MIR_OP_ZEXT);
    anvil_mir_vreg_t uses[] = {index};
    if (!anvil_mir_add_instr(lower->mir, op, normalized, uses, 1))
        return ANVIL_MIR_NO_VREG;
    return normalized;
}

static bool systemz_get_wide_const_value(systemz_lower_t *lower, anvil_value_t *value, int64_t *out)
{
    if (systemz_get_const_int(value, out))
        return true;
    return systemz_wide_const_get(lower, value, out);
}

static bool systemz_lower_store_i64_const(systemz_lower_t *lower, int64_t imm, anvil_mir_vreg_t addr)
{
    uint64_t bits = (uint64_t)imm;
    int32_t hi = (int32_t)(bits >> 32);
    int32_t lo = (int32_t)(bits & 0xffffffffu);

    anvil_mir_vreg_t hi_reg = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, true);
    anvil_mir_vreg_t lo_reg = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, true);
    anvil_mir_vreg_t lo_addr = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->ptr_size * 8), false);
    if (hi_reg == ANVIL_MIR_NO_VREG || lo_reg == ANVIL_MIR_NO_VREG || lo_addr == ANVIL_MIR_NO_VREG) {
        return false;
    }

    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, hi_reg, (int64_t)hi)) {
        return false;
    }
    anvil_mir_vreg_t hi_uses[] = {hi_reg, addr};
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, hi_uses, 2)) {
        return false;
    }

    if (!systemz_lower_add_const_offset(lower, addr, 4, lo_addr))
        return false;
    if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, lo_reg, (int64_t)lo)) {
        return false;
    }
    anvil_mir_vreg_t lo_uses[] = {lo_reg, lo_addr};
    return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, lo_uses, 2);
}

static anvil_mir_vreg_t systemz_add_i32_vreg(systemz_lower_t *lower, bool is_signed)
{
    return anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, is_signed);
}

static anvil_mir_vreg_t systemz_add_bool_vreg(systemz_lower_t *lower)
{
    return anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
}

static bool systemz_lower_load_i64_pair(systemz_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || instr->num_operands < 1)
        return false;
    anvil_mir_vreg_t addr = systemz_lower_value(lower, instr->operands[0]);
    if (addr == ANVIL_MIR_NO_VREG)
        return false;

    bool is_unsigned = instr->result->type && instr->result->type->kind == ANVIL_TYPE_U64;
    anvil_mir_vreg_t hi = systemz_add_i32_vreg(lower, !is_unsigned);
    anvil_mir_vreg_t lo = systemz_add_i32_vreg(lower, false);
    anvil_mir_vreg_t lo_addr = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->ptr_size * 8), false);
    if (hi == ANVIL_MIR_NO_VREG || lo == ANVIL_MIR_NO_VREG || lo_addr == ANVIL_MIR_NO_VREG) {
        return false;
    }

    anvil_mir_vreg_t hi_uses[] = {addr};
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_LOAD, hi, hi_uses, 1)) {
        return false;
    }
    if (!systemz_lower_add_const_offset(lower, addr, 4, lo_addr))
        return false;
    anvil_mir_vreg_t lo_uses[] = {lo_addr};
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_LOAD, lo, lo_uses, 1)) {
        return false;
    }
    return systemz_wide_pair_put(lower, instr->result, hi, lo, is_unsigned);
}

static bool systemz_lower_store_i64_pair(systemz_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t addr)
{
    anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
    if (!systemz_wide_pair_get(lower, value, &hi, &lo, NULL))
        return false;

    anvil_mir_vreg_t hi_uses[] = {hi, addr};
    if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, hi_uses, 2)) {
        return false;
    }

    anvil_mir_vreg_t lo_addr = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->ptr_size * 8), false);
    if (lo_addr == ANVIL_MIR_NO_VREG)
        return false;
    if (!systemz_lower_add_const_offset(lower, addr, 4, lo_addr))
        return false;
    anvil_mir_vreg_t lo_uses[] = {lo, lo_addr};
    return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, lo_uses, 2);
}

static bool systemz_lower_i64_bitcast_pair(systemz_lower_t *lower, anvil_instr_t *instr)
{
    anvil_mir_vreg_t hi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t lo = ANVIL_MIR_NO_VREG;
    if (!instr->result || instr->num_operands < 1 || !systemz_wide_pair_get(lower, instr->operands[0], &hi, &lo, NULL)) {
        return false;
    }
    bool is_unsigned = instr->result->type && instr->result->type->kind == ANVIL_TYPE_U64;
    return systemz_wide_pair_put(lower, instr->result, hi, lo, is_unsigned);
}

static bool systemz_add_pair_cmp(systemz_lower_t *lower, anvil_mir_opcode_t op, anvil_mir_vreg_t def, anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs)
{
    anvil_mir_vreg_t uses[] = {lhs, rhs};
    return anvil_mir_add_instr(lower->mir, op, def, uses, 2);
}

static bool systemz_lower_i64_cmp_pair(systemz_lower_t *lower, anvil_instr_t *instr, anvil_mir_opcode_t op)
{
    if (!instr->result || instr->num_operands < 2)
        return false;
    anvil_mir_vreg_t lhi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t llo = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t rhi = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t rlo = ANVIL_MIR_NO_VREG;
    if (!systemz_wide_pair_get(lower, instr->operands[0], &lhi, &llo, NULL) || !systemz_wide_pair_get(lower, instr->operands[1], &rhi, &rlo, NULL)) {
        return false;
    }

    anvil_mir_vreg_t result = systemz_add_bool_vreg(lower);
    if (result == ANVIL_MIR_NO_VREG)
        return false;

    if (op == ANVIL_MIR_OP_CMP_EQ || op == ANVIL_MIR_OP_CMP_NE) {
        anvil_mir_vreg_t hi_cmp = systemz_add_bool_vreg(lower);
        anvil_mir_vreg_t lo_cmp = systemz_add_bool_vreg(lower);
        if (hi_cmp == ANVIL_MIR_NO_VREG || lo_cmp == ANVIL_MIR_NO_VREG) {
            return false;
        }
        if (!systemz_add_pair_cmp(lower, op, hi_cmp, lhi, rhi) || !systemz_add_pair_cmp(lower, op, lo_cmp, llo, rlo)) {
            return false;
        }
        anvil_mir_vreg_t uses[] = {hi_cmp, lo_cmp};
        anvil_mir_opcode_t join = op == ANVIL_MIR_OP_CMP_EQ ? ANVIL_MIR_OP_AND : ANVIL_MIR_OP_OR;
        if (!anvil_mir_add_instr(lower->mir, join, result, uses, 2))
            return false;
        return systemz_map_put(lower, instr->result, result);
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

    anvil_mir_vreg_t hi_cmp = systemz_add_bool_vreg(lower);
    anvil_mir_vreg_t hi_eq = systemz_add_bool_vreg(lower);
    anvil_mir_vreg_t lo_cmp = systemz_add_bool_vreg(lower);
    anvil_mir_vreg_t eq_and_lo = systemz_add_bool_vreg(lower);
    if (hi_cmp == ANVIL_MIR_NO_VREG || hi_eq == ANVIL_MIR_NO_VREG || lo_cmp == ANVIL_MIR_NO_VREG || eq_and_lo == ANVIL_MIR_NO_VREG) {
        return false;
    }
    if (!systemz_add_pair_cmp(lower, hi_rel, hi_cmp, lhi, rhi) || !systemz_add_pair_cmp(lower, ANVIL_MIR_OP_CMP_EQ, hi_eq, lhi, rhi) || !systemz_add_pair_cmp(lower, lo_rel, lo_cmp, llo, rlo)) {
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
    return systemz_map_put(lower, instr->result, result);
}

static bool systemz_lower_gep(systemz_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || instr->num_operands < 2 || !instr->aux_type)
        return false;
    anvil_mir_vreg_t cur = systemz_lower_value(lower, instr->operands[0]);
    if (cur == ANVIL_MIR_NO_VREG)
        return false;

    anvil_type_t *walk_type = instr->aux_type;
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
        if (step.kind != ANVIL_GEP_STEP_SCALE)
            return false;
        int64_t elem_size = (int64_t)step.amount;
        anvil_mir_vreg_t idx = systemz_lower_value(lower, index_value);
        if (idx == ANVIL_MIR_NO_VREG)
            return false;
        idx = systemz_normalize_gep_index(lower, idx, index_value->type);
        if (idx == ANVIL_MIR_NO_VREG)
            return false;

        anvil_mir_vreg_t scaled = idx;
        if (elem_size != 1) {
            anvil_mir_vreg_t scale = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->ptr_size * 8), true);
            scaled = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, (uint16_t)(lower->desc->ptr_size * 8), true);
            if (scale == ANVIL_MIR_NO_VREG || scaled == ANVIL_MIR_NO_VREG) {
                return false;
            }
            if (!anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, scale, elem_size)) {
                return false;
            }
            anvil_mir_vreg_t mul_uses[] = {idx, scale};
            if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_MUL, scaled, mul_uses, 2)) {
                return false;
            }
        }

        anvil_mir_vreg_t next = systemz_add_vreg_for_type(lower, instr->result->type);
        if (next == ANVIL_MIR_NO_VREG)
            return false;
        anvil_mir_vreg_t add_uses[] = {cur, scaled};
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_ADD, next, add_uses, 2)) {
            return false;
        }
        cur = next;
    }

    if (constant_offset != 0) {
        anvil_mir_vreg_t next = systemz_add_vreg_for_type(lower, instr->result->type);
        if (next == ANVIL_MIR_NO_VREG || !systemz_lower_add_const_offset(lower, cur, constant_offset, next)) {
            return false;
        }
        cur = next;
    }

    return systemz_map_put(lower, instr->result, cur);
}

static bool systemz_lower_struct_gep(systemz_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr->result || instr->num_operands < 2 || !instr->aux_type || instr->aux_type->kind != ANVIL_TYPE_STRUCT || instr->operands[1]->kind != ANVIL_VAL_CONST_INT) {
        return false;
    }
    uint64_t field = instr->operands[1]->type && !instr->operands[1]->type->is_signed ? instr->operands[1]->data.u : (uint64_t)instr->operands[1]->data.i;
    if (field >= instr->aux_type->data.struc.num_fields)
        return false;

    anvil_mir_vreg_t base = systemz_lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t dst = systemz_add_vreg_for_type(lower, instr->result->type);
    if (base == ANVIL_MIR_NO_VREG || dst == ANVIL_MIR_NO_VREG)
        return false;

    if (!systemz_lower_add_const_offset(lower, base, (int64_t)instr->aux_type->data.struc.offsets[field], dst)) {
        return false;
    }
    return systemz_map_put(lower, instr->result, dst);
}

static bool systemz_lower_call(systemz_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands == 0)
        return false;
    if (instr->call_cc != ANVIL_CC_MVS)
        return false;

    size_t num_args = instr->num_operands - 1;
    anvil_mir_vreg_t *arg_vregs = num_args ? calloc(num_args, sizeof(*arg_vregs)) : NULL;
    if (num_args && !arg_vregs)
        return false;
    /* Materializing constants/symbols can itself append MIR. Resolve every
       value before starting the contiguous CALL_STACK_ARG bundle. */
    for (size_t i = 0; i < num_args; i++) {
        arg_vregs[i] = systemz_lower_value(lower, instr->operands[i + 1]);
        if (arg_vregs[i] == ANVIL_MIR_NO_VREG) {
            free(arg_vregs);
            return false;
        }
    }
    for (size_t i = 0; i < num_args; i++) {
        anvil_mir_vreg_t uses[] = {arg_vregs[i]};
        if (!anvil_mir_add_instr_imm_uses(lower->mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG, uses, 1, (int64_t)i)) {
            free(arg_vregs);
            return false;
        }
    }
    free(arg_vregs);

    anvil_mir_vreg_t call_def = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t result_vreg = ANVIL_MIR_NO_VREG;
    if (instr->result && !systemz_type_is_void(instr->result->type)) {
        call_def = systemz_add_vreg_for_type(lower, instr->result->type);
        if (call_def == ANVIL_MIR_NO_VREG)
            return false;
        if (!anvil_mir_set_fixed_reg(lower->mir, call_def, systemz_type_is_fp(instr->result->type) ? lower->desc->return_fpr : lower->desc->return_gpr)) {
            return false;
        }
    }

    bool ok = false;
    anvil_value_t *callee = instr->operands[0];
    if (callee->kind == ANVIL_VAL_FUNC) {
        ok = anvil_mir_add_call(lower->mir, call_def, NULL, 0, callee->name, instr->call_cc, false, 0);
    } else {
        anvil_mir_vreg_t callee_vreg = systemz_lower_value(lower, callee);
        if (callee_vreg == ANVIL_MIR_NO_VREG)
            return false;
        anvil_mir_vreg_t uses[] = {callee_vreg};
        ok = anvil_mir_add_call(lower->mir, call_def, uses, 1, NULL, instr->call_cc, false, 0);
    }
    if (!ok)
        return false;

    if (instr->result) {
        if (systemz_type_is_void(instr->result->type)) {
            return true;
        }
        result_vreg = systemz_add_vreg_for_type(lower, instr->result->type);
        if (result_vreg == ANVIL_MIR_NO_VREG)
            return false;
        anvil_mir_vreg_t uses[] = {call_def};
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, result_vreg, uses, 1)) {
            return false;
        }
        return systemz_map_put(lower, instr->result, result_vreg);
    }
    return true;
}

static bool systemz_lower_switch(systemz_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->num_operands == 0 || !instr->true_block)
        return false;
    anvil_mir_vreg_t selector = systemz_lower_value(lower, instr->operands[0]);
    if (selector == ANVIL_MIR_NO_VREG)
        return false;

    anvil_mir_block_t default_block = systemz_create_phi_edge_block(lower, instr->parent, instr->true_block);
    if (default_block == ANVIL_MIR_NO_BLOCK)
        return false;

    for (size_t i = 0; i < instr->num_switch_cases; i++) {
        if (!instr->switch_blocks || i + 1 >= instr->num_operands)
            return false;
        anvil_mir_block_t case_target = systemz_create_phi_edge_block(lower, instr->parent, instr->switch_blocks[i]);
        if (case_target == ANVIL_MIR_NO_BLOCK)
            return false;

        char block_name[96];
        snprintf(block_name, sizeof(block_name), "switch_case_%zu", anvil_mir_num_blocks(lower->mir));
        anvil_mir_block_t source = anvil_mir_current_block(lower->mir);
        anvil_mir_block_t next_block = anvil_mir_add_block(lower->mir, block_name);
        if (next_block == ANVIL_MIR_NO_BLOCK)
            return false;
        if (!anvil_mir_set_current_block(lower->mir, source))
            return false;

        anvil_mir_vreg_t case_val = systemz_lower_value(lower, instr->operands[i + 1]);
        anvil_mir_vreg_t cond = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
        if (case_val == ANVIL_MIR_NO_VREG || cond == ANVIL_MIR_NO_VREG) {
            return false;
        }
        anvil_mir_vreg_t cmp_uses[] = {selector, case_val};
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_CMP_EQ, cond, cmp_uses, 2)) {
            return false;
        }
        if (!anvil_mir_add_cond_branch(lower->mir, cond, case_target, next_block)) {
            return false;
        }
        if (!anvil_mir_set_current_block(lower->mir, next_block))
            return false;
    }

    return anvil_mir_add_branch(lower->mir, default_block);
}

static bool systemz_lower_instr(systemz_lower_t *lower, anvil_instr_t *instr)
{
    if (!instr)
        return true;
    if (instr->op == ANVIL_OP_PHI || instr->op == ANVIL_OP_NOP)
        return true;

    anvil_mir_opcode_t op;
    if (systemz_binary_op(instr->op, &op)) {
        if (!instr->result || instr->num_operands < 2)
            return false;
        if (!lower->desc->has_64bit_gprs && systemz_type_is_64bit_integer(instr->operands[0]->type) && systemz_type_is_64bit_integer(instr->operands[1]->type) &&
            (op == ANVIL_MIR_OP_CMP_EQ || op == ANVIL_MIR_OP_CMP_NE || op == ANVIL_MIR_OP_CMP_LT || op == ANVIL_MIR_OP_CMP_LE || op == ANVIL_MIR_OP_CMP_GT || op == ANVIL_MIR_OP_CMP_GE ||
             op == ANVIL_MIR_OP_CMP_ULT || op == ANVIL_MIR_OP_CMP_ULE || op == ANVIL_MIR_OP_CMP_UGT || op == ANVIL_MIR_OP_CMP_UGE)) {
            return systemz_lower_i64_cmp_pair(lower, instr, op);
        }
        anvil_mir_vreg_t lhs = systemz_lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t rhs = systemz_lower_value(lower, instr->operands[1]);
        anvil_mir_vreg_t def = systemz_add_vreg_for_type(lower, instr->result->type);
        if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) {
            return false;
        }
        anvil_mir_vreg_t uses[] = {lhs, rhs};
        bool added = op == ANVIL_MIR_OP_FCMP ? anvil_mir_add_instr_imm_uses(lower->mir, op, def, uses, 2, instr->fcmp_pred) : anvil_mir_add_instr(lower->mir, op, def, uses, 2);
        if (!added)
            return false;
        return systemz_map_put(lower, instr->result, def);
    }

    if (systemz_unary_op(instr->op, &op)) {
        if (!instr->result || instr->num_operands < 1)
            return false;
        if (!lower->desc->has_64bit_gprs && systemz_type_is_64bit_integer(instr->result->type)) {
            if (instr->op == ANVIL_OP_BITCAST) {
                return systemz_lower_i64_bitcast_pair(lower, instr);
            }
            int64_t imm = 0;
            if (!systemz_get_wide_const_value(lower, instr->operands[0], &imm)) {
                return false;
            }
            if (instr->op == ANVIL_OP_NEG) {
                return systemz_wide_const_put(lower, instr->result, (int64_t)(0 - (uint64_t)imm));
            }
            if (instr->op == ANVIL_OP_NOT) {
                return systemz_wide_const_put(lower, instr->result, ~imm);
            }
            return false;
        }
        anvil_mir_vreg_t src = systemz_lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t def = systemz_add_vreg_for_type(lower, instr->result->type);
        if (src == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG)
            return false;
        if (instr->result->type->kind == ANVIL_TYPE_I1 && instr->operands[0]->type->kind != ANVIL_TYPE_I1) {
            const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(lower->mir, src);
            if (src_info && src_info->reg_class == ANVIL_MIR_REG_FPR) {
                if (op != ANVIL_MIR_OP_FPTOUI)
                    return false;
                anvil_mir_vreg_t converted = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, false);
                anvil_mir_vreg_t convert_use[] = {src};
                if (converted == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(lower->mir, op, converted, convert_use, 1))
                    return false;
                src = converted;
                src_info = anvil_mir_get_vreg_info(lower->mir, src);
            }
            if (!src_info || src_info->reg_class != ANVIL_MIR_REG_GPR)
                return false;
            anvil_mir_vreg_t narrowed = src;
            if (src_info->size_bits > 8) {
                narrowed = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
                anvil_mir_vreg_t narrow_use[] = {src};
                if (narrowed == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_TRUNC, narrowed, narrow_use, 1))
                    return false;
            }
            anvil_mir_vreg_t one = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
            anvil_mir_vreg_t norm_uses[] = {narrowed, one};
            if (one == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, one, 1) || !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_AND, def, norm_uses, 2))
                return false;
            return systemz_map_put(lower, instr->result, def);
        }
        anvil_mir_vreg_t uses[] = {src};
        if (!anvil_mir_add_instr(lower->mir, op, def, uses, 1))
            return false;
        return systemz_map_put(lower, instr->result, def);
    }

    switch (instr->op) {
    case ANVIL_OP_ALLOCA:
        return systemz_lower_alloca(lower, instr);

    case ANVIL_OP_GEP:
        return systemz_lower_gep(lower, instr);

    case ANVIL_OP_STRUCT_GEP:
        return systemz_lower_struct_gep(lower, instr);

    case ANVIL_OP_LOAD: {
        if (!instr->result || instr->num_operands < 1)
            return false;
        if (!lower->desc->has_64bit_gprs && systemz_type_is_64bit_integer(instr->result->type)) {
            return systemz_lower_load_i64_pair(lower, instr);
        }
        anvil_mir_vreg_t addr = systemz_lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t def = systemz_add_vreg_for_type(lower, instr->result->type);
        if (addr == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG)
            return false;
        bool is_i1 = instr->result->type->kind == ANVIL_TYPE_I1;
        anvil_mir_vreg_t loaded = is_i1 ? anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false) : def;
        if (loaded == ANVIL_MIR_NO_VREG)
            return false;
        anvil_mir_vreg_t uses[] = {addr};
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_LOAD, loaded, uses, 1)) {
            return false;
        }
        if (is_i1) {
            anvil_mir_vreg_t one = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
            anvil_mir_vreg_t norm_uses[] = {loaded, one};
            if (one == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, one, 1) || !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_AND, def, norm_uses, 2))
                return false;
        }
        return systemz_map_put(lower, instr->result, def);
    }

    case ANVIL_OP_STORE: {
        if (instr->num_operands < 2)
            return false;
        if (!lower->desc->has_64bit_gprs && systemz_type_is_64bit_integer(instr->operands[0]->type)) {
            int64_t imm = 0;
            if (systemz_get_wide_const_value(lower, instr->operands[0], &imm)) {
                anvil_mir_vreg_t addr = systemz_lower_value(lower, instr->operands[1]);
                if (addr == ANVIL_MIR_NO_VREG)
                    return false;
                return systemz_lower_store_i64_const(lower, imm, addr);
            }
            if (systemz_wide_pair_get(lower, instr->operands[0], NULL, NULL, NULL)) {
                anvil_mir_vreg_t addr = systemz_lower_value(lower, instr->operands[1]);
                if (addr == ANVIL_MIR_NO_VREG)
                    return false;
                return systemz_lower_store_i64_pair(lower, instr->operands[0], addr);
            }
        }
        anvil_mir_vreg_t value = systemz_lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t addr = systemz_lower_value(lower, instr->operands[1]);
        if (value == ANVIL_MIR_NO_VREG || addr == ANVIL_MIR_NO_VREG)
            return false;
        if (instr->operands[0]->type->kind == ANVIL_TYPE_I1) {
            anvil_mir_vreg_t one = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
            anvil_mir_vreg_t normalized = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 8, false);
            anvil_mir_vreg_t norm_uses[] = {value, one};
            if (one == ANVIL_MIR_NO_VREG || normalized == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, one, 1) ||
                !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_AND, normalized, norm_uses, 2))
                return false;
            value = normalized;
        }
        anvil_mir_vreg_t uses[] = {value, addr};
        return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, uses, 2);
    }

    case ANVIL_OP_CALL:
        return systemz_lower_call(lower, instr);

    case ANVIL_OP_RET:
        return systemz_add_return_copy(lower, instr->num_operands ? instr->operands[0] : NULL);

    case ANVIL_OP_BR: {
        anvil_mir_block_t target = systemz_create_phi_edge_block(lower, instr->parent, instr->true_block);
        return target != ANVIL_MIR_NO_BLOCK && anvil_mir_add_branch(lower->mir, target);
    }

    case ANVIL_OP_BR_COND: {
        if (instr->num_operands < 1)
            return false;
        anvil_mir_vreg_t cond = systemz_lower_value(lower, instr->operands[0]);
        anvil_mir_block_t true_block = systemz_create_phi_edge_block(lower, instr->parent, instr->true_block);
        anvil_mir_block_t false_block = systemz_create_phi_edge_block(lower, instr->parent, instr->false_block);
        return cond != ANVIL_MIR_NO_VREG && true_block != ANVIL_MIR_NO_BLOCK && false_block != ANVIL_MIR_NO_BLOCK && anvil_mir_add_cond_branch(lower->mir, cond, true_block, false_block);
    }

    case ANVIL_OP_SWITCH:
        return systemz_lower_switch(lower, instr);

    case ANVIL_OP_SELECT: {
        if (!instr->result || instr->num_operands < 3)
            return false;
        anvil_mir_vreg_t cond = systemz_lower_value(lower, instr->operands[0]);
        anvil_mir_vreg_t tv = systemz_lower_value(lower, instr->operands[1]);
        anvil_mir_vreg_t fv = systemz_lower_value(lower, instr->operands[2]);
        anvil_mir_vreg_t def = systemz_add_vreg_for_type(lower, instr->result->type);
        if (cond == ANVIL_MIR_NO_VREG || tv == ANVIL_MIR_NO_VREG || fv == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG) {
            return false;
        }
        anvil_mir_vreg_t uses[] = {cond, tv, fv};
        if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_SELECT, def, uses, 3)) {
            return false;
        }
        return systemz_map_put(lower, instr->result, def);
    }

    default:
        return false;
    }
}

anvil_mir_func_t *anvil_mainframe_lower_func_to_mir(anvil_func_t *func, anvil_mainframe_variant_t variant)
{
    const anvil_mainframe_target_desc_t *desc = anvil_mainframe_get_target_desc(variant);
    if (!func || !desc || !func->entry || !func->type || func->type->kind != ANVIL_TYPE_FUNC || func->type->data.func.cc != ANVIL_CC_MVS)
        return NULL;

    anvil_mir_func_t *mir = anvil_mir_func_create(func->name);
    if (!mir)
        return NULL;

    systemz_lower_t lower;
    memset(&lower, 0, sizeof(lower));
    lower.desc = desc;
    lower.func = func;
    lower.mir = mir;

    anvil_opt_cfg_t cfg;
    bool ok = anvil_opt_cfg_build(func, &cfg);
    if (ok)
        ok = systemz_create_mir_blocks(&lower);
    if (ok)
        ok = anvil_mir_set_current_block(mir, systemz_block_get(&lower, func->entry));
    if (ok)
        ok = systemz_lower_params(&lower);
    if (ok)
        ok = systemz_prepare_phi_results(&lower);
    if (ok)
        ok = systemz_materialize_entry_values(&lower);

    for (size_t rank = 0; ok && rank < cfg.count; rank++) {
        anvil_block_t *block = cfg.blocks[cfg.rpo[rank]];
        ok = anvil_mir_set_current_block(mir, systemz_block_get(&lower, block));
        for (anvil_instr_t *instr = block->first; ok && instr; instr = instr->next) {
            size_t first = anvil_mir_num_instrs(lower.mir);
            ok = systemz_lower_instr(&lower, instr) && anvil_mir_annotate_memory(lower.mir, first, &instr->memory_access);
            if (ok && instr->op == ANVIL_OP_CALL)
                ok = anvil_mir_annotate_call_effects(lower.mir, first, anvil_instr_call_effects(instr));
        }
    }

    anvil_opt_cfg_destroy(&cfg);
    free(lower.values);
    free(lower.blocks);
    free(lower.frame_addrs);
    free(lower.wide_consts);
    free(lower.wide_pairs);

    char verify_error[256] = {0};
    bool verified = ok && anvil_mir_verify(mir, verify_error, sizeof(verify_error));
    if (!verified) {
        if (func->parent && func->parent->ctx) {
            anvil_set_error(func->parent->ctx, ANVIL_ERR_CODEGEN, "Mainframe MIR lowering failed: %s", ok ? verify_error : "unsupported IR operation");
        }
        anvil_mir_func_destroy(mir);
        return NULL;
    }
    return mir;
}
