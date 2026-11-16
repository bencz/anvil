#include "systemz_internal.h"
#include "../common/ir_lower.h"
#include "../common/mir_legal.h"

bool systemz_is_call_preparation(const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *instr)
{
    return instr->op == ANVIL_MIR_OP_SPILL_LOAD || anvil_mir_instr_recomputable(mir, index);
}

static bool systemz_verify_call_bundles(const anvil_mir_func_t *mir, char *error, size_t error_len)
{
    size_t num_instrs = anvil_mir_num_instrs(mir);
    for (size_t i = 0; i < num_instrs; i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, i, &instr)) {
            return anvil_format_failure(error, error_len, "cannot inspect instruction %zu", i);
        }
        if (instr.op != ANVIL_MIR_OP_CALL_STACK_ARG)
            continue;

        size_t expected = 0;
        anvil_mir_block_t block = instr.block;
        for (;;) {
            if (!instr.has_imm || instr.imm != (int64_t)expected || instr.num_uses != 1 || instr.block != block) {
                return anvil_format_failure(error, error_len, "malformed call argument bundle at instruction %zu", i);
            }
            expected++;
            i++;
            /* Reloads and rematerialized constants may prepare the next
               argument inside the logical call bundle. */
            while (i < num_instrs && anvil_mir_get_instr_info(mir, i, &instr) && instr.block == block && systemz_is_call_preparation(mir, i, &instr)) {
                i++;
            }
            if (i >= num_instrs || !anvil_mir_get_instr_info(mir, i, &instr)) {
                return anvil_format_failure(error, error_len, "unterminated call argument bundle");
            }
            if (instr.op != ANVIL_MIR_OP_CALL_STACK_ARG || instr.block != block) {
                break;
            }
        }

        if (instr.op != ANVIL_MIR_OP_CALL || instr.block != block) {
            return anvil_format_failure(error, error_len, "call argument bundle is not followed by its call");
        }
    }
    return true;
}

static bool systemz_legal_hook(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *instr,
                               char *error, size_t error_len, bool *decided)
{
    const anvil_mainframe_target_desc_t *desc = target->user;
    *decided = true;
    switch (instr->op) {
    case ANVIL_MIR_OP_CALL:
        /* Arguments travel in the parameter list (the call bundle). */
        return instr->call_cc == ANVIL_CC_MVS || anvil_format_failure(error, error_len, "%s call uses an unsupported calling convention", desc->name);
    case ANVIL_MIR_OP_CALL_RESULT: {
        /* The low word of a pair result (see systemz_abi_ops_t). */
        const anvil_mir_vreg_info_t *def = anvil_mir_get_vreg_info(mir, instr->def);
        anvil_mir_instr_info_t call;
        if (desc->abi_ops->pair_low_gpr < 0 || !def || def->reg_class != ANVIL_MIR_REG_GPR || def->size_bits != 32 || index == 0 ||
            !anvil_mir_get_instr_info(mir, index - 1, &call) || call.op != ANVIL_MIR_OP_CALL || call.def == ANVIL_MIR_NO_VREG)
            return anvil_format_failure(error, error_len, "%s call result %zu must be the low word of a pair result", desc->name, index);
        return true;
    }
    case ANVIL_MIR_OP_RET_VALUE_PART:
        return desc->abi_ops->pair_low_gpr >= 0 || anvil_format_failure(error, error_len, "%s cannot return a word pair", desc->name);
    case ANVIL_MIR_OP_LOAD:
    case ANVIL_MIR_OP_STORE:
        *decided = false;
        if (instr->has_imm && !systemz_displacement_fits(instr->imm, anvil_mir_func_cpu_features(mir)))
            return anvil_format_failure(error, error_len, "%s memory displacement %lld is out of range", desc->name, (long long)instr->imm);
        return true;
    default:
        *decided = false;
        return true;
    }
}

bool anvil_mainframe_verify_mir_legal(const anvil_mir_func_t *mir, anvil_mainframe_variant_t variant, char *error, size_t error_len)
{
    const anvil_mainframe_target_desc_t *desc = anvil_mainframe_get_target_desc(variant);
    if (!desc || !mir)
        return anvil_format_failure(error, error_len, "invalid mainframe MIR input");
    uint8_t gpr_widths = ANVIL_MIR_WIDTH(8) | ANVIL_MIR_WIDTH(16) | ANVIL_MIR_WIDTH(32) | (desc->has_64bit_gprs ? ANVIL_MIR_WIDTH(64) : 0);
    anvil_mir_legal_target_t target = {
        .name = desc->name,
        .widths = {[ANVIL_MIR_REG_GPR] = gpr_widths, [ANVIL_MIR_REG_FPR] = ANVIL_MIR_WIDTH(32) | ANVIL_MIR_WIDTH(64)},
        .num_regs = {[ANVIL_MIR_REG_GPR] = 16, [ANVIL_MIR_REG_FPR] = 16},
        .pointer_bits = (uint16_t)(desc->ptr_size * 8),
        /* Stack-argument immediates number the parameter-list entries. */
        .stack_arg_align = 1,
        .select_classes = ANVIL_IR_SELECT_EVERY_CLASS,
        .hook = systemz_legal_hook,
        .user = desc,
    };
    target.scalar_isa = systemz_scalar_table(desc, &target.scalar_isa_count);
    return anvil_mir_verify_legal(&target, mir, error, error_len) && systemz_verify_call_bundles(mir, error, error_len);
}

enum { SYSTEMZ_GPR_ACCESS_MAX = 8 };

bool anvil_systemz_indexed_displacement(anvil_backend_t *be, int64_t displacement, size_t access_bytes)
{
    if (!be || !be->ctx)
        return false;
    return access_bytes > SYSTEMZ_GPR_ACCESS_MAX ? systemz_short_displacement(displacement)
                                                 : systemz_displacement_fits(displacement, anvil_ctx_get_cpu_features(be->ctx));
}

/* D(X,B): base + index + a displacement (systemz_displacement_fits for
 * the CPU features of the function `user` is). */
static bool systemz_address_mode(void *user, unsigned scale, int64_t disp, uint16_t access_bits, anvil_mir_index_extend_t extend)
{
    (void)access_bits;
    return extend == ANVIL_MIR_INDEX_PLAIN && scale == 1 && systemz_displacement_fits(disp, anvil_mir_func_cpu_features(user));
}

/* LHI takes a signed 16-bit value; anything else (floating constants
 * included) is a literal-pool load. */
static bool systemz_expensive_constant(void *user, anvil_mir_reg_class_t reg_class, uint16_t size_bits, int64_t value)
{
    (void)user;
    (void)size_bits;
    return reg_class != ANVIL_MIR_REG_GPR || !systemz_simm16(value);
}

bool anvil_mainframe_regalloc_mir(anvil_mir_func_t *mir, anvil_mainframe_variant_t variant)
{
    const anvil_mainframe_target_desc_t *desc = anvil_mainframe_get_target_desc(variant);
    if (!desc)
        return false;
    return anvil_mainframe_regalloc_mir_features(mir, variant, anvil_cpu_model_features(anvil_arch_default_cpu(desc->arch)));
}

typedef struct {
    const anvil_mainframe_target_desc_t *desc;
    anvil_mainframe_variant_t variant;
    anvil_cpu_features_t features;
} systemz_allocation_t;

static bool systemz_allocation_verify(const anvil_mir_func_t *mir, const void *user)
{
    const systemz_allocation_t *target = user;
    return anvil_mainframe_verify_mir_legal(mir, target->variant, NULL, 0);
}

static bool systemz_allocation_registerless(anvil_mir_func_t *mir, const void *user)
{
    const systemz_allocation_t *target = user;
    return anvil_systemz_mark_registerless(mir, target->desc, target->features);
}

bool anvil_mainframe_regalloc_mir_features(anvil_mir_func_t *mir, anvil_mainframe_variant_t variant, anvil_cpu_features_t features)
{
    const anvil_mainframe_target_desc_t *desc = anvil_mainframe_get_target_desc(variant);
    if (!desc)
        return false;
    systemz_allocation_t target = {desc, variant, features};

    /* The FP register file depends on the CPU model (see systemz_target.c). */
    bool afp = (features & ANVIL_FEATURE_ZARCH_BFP) != 0;
    const int *fprs = afp ? systemz_s390_fprs : systemz_s370_fprs;
    int num_fprs = afp ? (int)(sizeof(systemz_s390_fprs) / sizeof(int)) : (int)(sizeof(systemz_s370_fprs) / sizeof(int));
    const int *scratch_fprs = afp ? systemz_s390_scratch_fprs : systemz_s370_scratch_fprs;
    int num_scratch_fprs = afp ? (int)(sizeof(systemz_s390_scratch_fprs) / sizeof(int)) : (int)(sizeof(systemz_s370_scratch_fprs) / sizeof(int));
    /* Arithmetic overwrites its first operand (AR/AGR/AHI, ADBR...). */
    anvil_regalloc_class_config_t configs[] = {
        {.reg_class = ANVIL_MIR_REG_GPR, .num_phys_regs = (int)desc->num_alloc_gpr_regs, .phys_regs = desc->alloc_gpr_regs, .two_address = true},
        {.reg_class = ANVIL_MIR_REG_FPR, .num_phys_regs = num_fprs, .phys_regs = fprs, .two_address = true}};
    /* hlasm.c's temporaries (R0, R1, R14, R15, F0) are outside the pools:
     * it writes pool registers only as results or across calls. */
    anvil_regalloc_class_config_t scratch_configs[] = {{.reg_class = ANVIL_MIR_REG_GPR, .num_phys_regs = (int)desc->num_scratch_gpr_regs,
                                                        .phys_regs = desc->scratch_gpr_regs, .printer_declares_writes = true},
                                                       {.reg_class = ANVIL_MIR_REG_FPR, .num_phys_regs = num_scratch_fprs, .phys_regs = scratch_fprs,
                                                        .printer_declares_writes = true}};
    anvil_mir_allocation_t allocation = {
        .verify = systemz_allocation_verify,
        .address_mode = systemz_address_mode,
        .expensive_constant = systemz_expensive_constant,
        .mir_user = mir,
        .mark_registerless = systemz_allocation_registerless,
        .configs = configs,
        .num_configs = sizeof(configs) / sizeof(configs[0]),
        .scratch_configs = scratch_configs,
        .num_scratch_configs = sizeof(scratch_configs) / sizeof(scratch_configs[0]),
        .user = &target,
    };
    return anvil_mir_allocate(mir, &allocation);
}
