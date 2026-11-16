#include "ppc_internal.h"
#include "../common/ir_lower.h"
#include "../common/mir_legal.h"

static bool ppc_reg_is_in_set(int reg, const int *regs, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (regs[i] == reg)
            return true;
    }
    return false;
}

static bool ppc_legal_result(const anvil_mir_legal_target_t *target, const anvil_mir_vreg_info_t *info)
{
    const anvil_ppc_target_desc_t *desc = target->user;
    return info->fixed_phys_reg == (info->reg_class == ANVIL_MIR_REG_FPR ? desc->fpr_return_reg : desc->gpr_return_reg);
}

static bool ppc_legal_argument(const anvil_mir_legal_target_t *target, const anvil_mir_vreg_info_t *info)
{
    const anvil_ppc_target_desc_t *desc = target->user;
    if (info->reg_class == ANVIL_MIR_REG_GPR)
        return ppc_reg_is_in_set(info->fixed_phys_reg, desc->gpr_arg_regs, desc->num_gpr_arg_regs);
    return info->reg_class == ANVIL_MIR_REG_FPR && ppc_reg_is_in_set(info->fixed_phys_reg, desc->fpr_arg_regs, desc->num_fpr_arg_regs);
}

static bool ppc_legal_hook(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *instr, char *error,
                           size_t error_len, bool *decided)
{
    const anvil_ppc_target_desc_t *desc = target->user;
    *decided = true;
    switch (instr->op) {
    case ANVIL_MIR_OP_CALL: {
        anvil_mir_legal_call_t rules = {.fixed_result = true, .result_ok = ppc_legal_result, .fixed_target = true, .target_reg = desc->indirect_call_reg,
                                        .fixed_args = true, .arg_ok = ppc_legal_argument};
        if (instr->call_cc != ANVIL_CC_SYSV)
            return anvil_format_failure(error, error_len, "PowerPC MIR call %zu uses an unsupported calling convention", index);
        return anvil_mir_legal_call(target, &rules, mir, index, instr, error, error_len);
    }
    case ANVIL_MIR_OP_VA_START:
        return anvil_mir_legal_pointer(target, mir, instr->def, index, error, error_len);
    case ANVIL_MIR_OP_CALL_RESULT: {
        const anvil_mir_vreg_info_t *def = anvil_mir_get_vreg_info(mir, instr->def);
        /* Second result register: r4 (64-bit pair or two-doubleword
         * aggregate) or f2-f8 (floating aggregate). */
        bool gpr = def && def->reg_class == ANVIL_MIR_REG_GPR && def->size_bits == desc->word_size * 8 && def->fixed_phys_reg == desc->gpr_return_reg + 1;
        bool fpr = def && def->reg_class == ANVIL_MIR_REG_FPR && def->fixed_phys_reg > desc->fpr_return_reg && def->fixed_phys_reg <= desc->fpr_return_reg + 7;
        if (!instr->symbol && instr->num_uses == 0 && def && def->has_fixed_reg && (gpr || fpr))
            return true;
        return anvil_format_failure(error, error_len, "PowerPC MIR instruction %zu has an invalid call-result pseudo", index);
    }
    case ANVIL_MIR_OP_LOAD:
    case ANVIL_MIR_OP_STORE:
        /* X-forms: base + index, unscaled, no displacement. */
        *decided = false;
        if (instr->index_scale && (instr->index_scale != 1 || (instr->has_imm && instr->imm != 0)))
            return anvil_format_failure(error, error_len, "PowerPC MIR access %zu: indexed forms take neither scale nor displacement", index);
        return true;
    default:
        *decided = false;
        return true;
    }
}

bool anvil_ppc_verify_mir_legal(const anvil_mir_func_t *mir, anvil_ppc_variant_t variant, char *error, size_t error_len)
{
    const anvil_ppc_target_desc_t *desc = anvil_ppc_get_target_desc(variant);
    if (!desc)
        return anvil_format_failure(error, error_len, "unknown PowerPC variant");
    uint8_t gpr_widths = ANVIL_MIR_WIDTH(8) | ANVIL_MIR_WIDTH(16) | ANVIL_MIR_WIDTH(32) | (desc->word_size == 8 ? ANVIL_MIR_WIDTH(64) : 0);
    anvil_mir_legal_target_t target = {
        .name = "PowerPC",
        .widths = {[ANVIL_MIR_REG_GPR] = gpr_widths, [ANVIL_MIR_REG_FPR] = ANVIL_MIR_WIDTH(32) | ANVIL_MIR_WIDTH(64),
                   [ANVIL_MIR_REG_VECTOR] = ANVIL_MIR_WIDTH(128)},
        .num_regs = {[ANVIL_MIR_REG_GPR] = 32, [ANVIL_MIR_REG_FPR] = 32, [ANVIL_MIR_REG_VECTOR] = 32},
        .pointer_bits = (uint16_t)(desc->word_size * 8),
        /* Word aligned; a 64-bit ABI single float sits in the low word of its doubleword. */
        .stack_arg_align = 4,
        .select_classes = ANVIL_IR_SELECT_EVERY_CLASS,
        .scalar_isa = ppc_vector_isa,
        .scalar_isa_count = ppc_vector_isa_count(desc),
        .vector_isa = ppc_vector_isa,
        .vector_isa_count = ppc_vector_isa_count(desc),
        .hook = ppc_legal_hook,
        .user = desc,
    };
    return anvil_mir_verify_legal(&target, mir, error, error_len);
}

/* Calls, and conversions the printer turns into helper calls, end the
 * values in volatile registers; atomics declared their own temporaries. */
static bool ppc_add_call_clobbers(anvil_mir_func_t *mir, const anvil_ppc_target_desc_t *desc, anvil_cpu_features_t features)
{
    const ppc_abi_ops_t *abi = desc->abi_ops;
    for (size_t index = 0; index < anvil_mir_num_instrs(mir); index++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, index, &info))
            return false;
        if (info.op != ANVIL_MIR_OP_CALL && !ppc_conversion_calls_helper(mir, index, &info, features))
            continue;
        if (!anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_GPR, info.clobbers[ANVIL_MIR_REG_GPR] | abi->call_gpr_clobbers) ||
            !anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_FPR, info.clobbers[ANVIL_MIR_REG_FPR] | abi->call_fpr_clobbers) ||
            !anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_VECTOR, info.clobbers[ANVIL_MIR_REG_VECTOR] | abi->call_vector_clobbers))
            return false;
    }
    return true;
}

/* X-form accesses: base + index, unscaled, no displacement. */
static bool ppc_address_mode(void *user, unsigned scale, int64_t disp, uint16_t access_bits, anvil_mir_index_extend_t extend)
{
    (void)user;
    (void)access_bits;
    return extend == ANVIL_MIR_INDEX_PLAIN && scale == 1 && disp == 0;
}

/* li takes a signed 16-bit value, lis one whose low half is zero within
 * 32 signed bits; floating constants go through r11 and the frame. */
static bool ppc_expensive_constant(void *user, anvil_mir_reg_class_t reg_class, uint16_t size_bits, int64_t value)
{
    (void)user;
    (void)size_bits;
    if (reg_class != ANVIL_MIR_REG_GPR)
        return true;
    return !(value >= -32768 && value <= 32767) && !((value & 0xffff) == 0 && value >= INT32_MIN && value <= INT32_MAX);
}

bool anvil_ppc_regalloc_mir(anvil_mir_func_t *mir, anvil_ppc_variant_t variant)
{
    /* Without a CPU model every conversion may call a helper. */
    return anvil_ppc_regalloc_mir_features(mir, variant, 0);
}

typedef struct {
    const anvil_ppc_target_desc_t *desc;
    anvil_ppc_variant_t variant;
    anvil_cpu_features_t features;
} ppc_allocation_t;

static bool ppc_allocation_verify(const anvil_mir_func_t *mir, const void *user)
{
    const ppc_allocation_t *target = user;
    return anvil_ppc_verify_mir_legal(mir, target->variant, NULL, 0);
}

static bool ppc_allocation_clobbers(anvil_mir_func_t *mir, const void *user)
{
    const ppc_allocation_t *target = user;
    return ppc_add_call_clobbers(mir, target->desc, target->features);
}

static bool ppc_allocation_printer(anvil_mir_func_t *mir, const void *user)
{
    const ppc_allocation_t *target = user;
    return ppc_annotate_printer_scratch(mir, target->features);
}

static bool ppc_allocation_registerless(anvil_mir_func_t *mir, const void *user)
{
    const ppc_allocation_t *target = user;
    return anvil_ppc_mark_registerless(mir, target->desc, target->features);
}

bool anvil_ppc_regalloc_mir_features(anvil_mir_func_t *mir, anvil_ppc_variant_t variant, anvil_cpu_features_t features)
{
    const anvil_ppc_target_desc_t *desc = anvil_ppc_get_target_desc(variant);
    if (!desc)
        return false;
    ppc_allocation_t target = {desc, variant, features};

    /* f14-f31 are nonvolatile only as FPRs: the VSX upper halves are not.
     * Vectors live in the AltiVec registers (vs32-vs63), volatile ones only;
     * v0 and v1 are the printer's temporaries. */
    anvil_regalloc_class_config_t configs[] = {
        {.reg_class = ANVIL_MIR_REG_GPR, .num_phys_regs = (int)desc->num_alloc_gpr_regs, .phys_regs = desc->alloc_gpr_regs},
        {.reg_class = ANVIL_MIR_REG_FPR, .num_phys_regs = (int)desc->num_alloc_fpr_regs, .phys_regs = desc->alloc_fpr_regs,
         .partially_preserved = ~UINT64_C(0) << 14 & UINT64_C(0xFFFFFFFF), .preserved_bits = 64},
        {.reg_class = ANVIL_MIR_REG_VECTOR, .num_phys_regs = PPC_NUM_ALLOC_VECTOR_REGS, .phys_regs = ppc_alloc_vector_regs},
    };
    /* ppc_emit.c declares every write to the GPR and FPR scratch registers
     * (ppc_annotate_printer_scratch, call and atomic clobbers). */
    anvil_regalloc_class_config_t scratch_configs[] = {
        {.reg_class = ANVIL_MIR_REG_GPR, .num_phys_regs = (int)desc->num_scratch_gpr_regs, .phys_regs = desc->scratch_gpr_regs,
         .printer_declares_writes = true},
        {.reg_class = ANVIL_MIR_REG_FPR, .num_phys_regs = (int)desc->num_scratch_fpr_regs, .phys_regs = desc->scratch_fpr_regs,
         .printer_declares_writes = true},
        {.reg_class = ANVIL_MIR_REG_VECTOR, .num_phys_regs = PPC_NUM_SCRATCH_VECTOR_REGS, .phys_regs = ppc_scratch_vector_regs},
    };
    anvil_mir_allocation_t allocation = {
        .verify = ppc_allocation_verify,
        .annotate_clobbers = ppc_allocation_clobbers,
        .address_mode = ppc_address_mode,
        .expensive_constant = ppc_expensive_constant,
        .annotate_printer = ppc_allocation_printer,
        .mark_registerless = ppc_allocation_registerless,
        .configs = configs,
        .num_configs = sizeof(configs) / sizeof(configs[0]),
        .scratch_configs = scratch_configs,
        .num_scratch_configs = sizeof(scratch_configs) / sizeof(scratch_configs[0]),
        .user = &target,
    };
    return anvil_mir_allocate(mir, &allocation);
}
