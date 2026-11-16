#include "x86_32_internal.h"
#include "x86_address.h"
#include "asm/x86_sse_vector.h"
#include "anvil/anvil_analysis.h"
#include "../common/mir_legal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool x86_legal_hook(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *instr, char *error,
                           size_t error_len, bool *decided)
{
    /* Stack arguments: neither the result nor the arguments are fixed. */
    static const anvil_mir_legal_call_t call_rules = {.target_reg = -1};
    *decided = true;
    switch (instr->op) {
    case ANVIL_MIR_OP_CALL:
        if (!anvil_x86_get_cc_desc(instr->call_cc))
            return anvil_format_failure(error, error_len, "x86 MIR call %zu uses an unsupported calling convention", index);
        return anvil_mir_legal_call(target, &call_rules, mir, index, instr, error, error_len);
    case ANVIL_MIR_OP_VA_START: {
        /* Pointer to the first unnamed stack argument. */
        const anvil_mir_vreg_info_t *def = anvil_mir_get_vreg_info(mir, instr->def);
        if (instr->has_imm && instr->imm >= 0 && instr->imm % 4 == 0 && def && def->reg_class == ANVIL_MIR_REG_GPR && def->size_bits == 32)
            return true;
        return anvil_format_failure(error, error_len, "x86 MIR va_start %zu needs a pointer result and a word-aligned argument offset", index);
    }
    case ANVIL_MIR_OP_CALL_RESULT: {
        /* The emitter copies the call's second result location (EDX for a
         * 32-bit value, ST(0) for a floating one) into the assigned
         * register (allocated, or a spill scratch) right after the call. */
        const anvil_mir_vreg_info_t *def = anvil_mir_get_vreg_info(mir, instr->def);
        if (def && ((def->reg_class == ANVIL_MIR_REG_GPR && def->size_bits == 32) || def->reg_class == ANVIL_MIR_REG_FPR))
            return true;
        return anvil_format_failure(error, error_len, "x86 MIR call result %zu must be a 32-bit or floating value", index);
    }
    case ANVIL_MIR_OP_RET_VALUE_PART:
        return true;
    case ANVIL_MIR_OP_VECTOR:
        *decided = false;
        return x86_has_vectors(anvil_mir_func_cpu_features(mir)) ||
               anvil_format_failure(error, error_len, "x86 MIR vector instruction %zu has no SSE2 unit on this CPU", index);
    default:
        *decided = false;
        return true;
    }
}

bool anvil_x86_verify_mir_legal(const anvil_mir_func_t *mir, char *error, size_t error_len)
{
    anvil_mir_legal_target_t target = {
        .name = "x86",
        /* 128-bit FP values: vectors in XMM registers (SSE2 CPUs). */
        .widths = {[ANVIL_MIR_REG_GPR] = ANVIL_MIR_WIDTH(8) | ANVIL_MIR_WIDTH(16) | ANVIL_MIR_WIDTH(32),
                   [ANVIL_MIR_REG_FPR] = ANVIL_MIR_WIDTH(32) | ANVIL_MIR_WIDTH(64) | ANVIL_MIR_WIDTH(128)},
        .num_regs = {[ANVIL_MIR_REG_GPR] = X86_NUM_GPR, [ANVIL_MIR_REG_FPR] = X86_NUM_FPR},
        .pointer_bits = 32,
        .fp_vector_bits = 128,
        .stack_arg_align = 4,
        .scalar_isa = x86_scalar_isa,
        .scalar_isa_count = x86_scalar_isa_count,
        .vector_isa = x86_sse_isa,
        .vector_isa_count = x86_sse_isa_count,
        .hook = x86_legal_hook,
    };
    /* CMOVcc (P6 and later) selects general registers; otherwise a select branches. */
    if (mir && (anvil_mir_func_cpu_features(mir) & ANVIL_FEATURE_X86_CMOV))
        target.select_classes = 1u << ANVIL_MIR_REG_GPR;
    return anvil_mir_verify_legal(&target, mir, error, error_len);
}

bool x86_keeps_frame_pointer(const anvil_mir_func_t *mir)
{
    for (size_t index = 0; index < anvil_mir_num_instrs(mir); index++) {
        anvil_mir_instr_info_t info;
        if (anvil_mir_get_instr_info(mir, index, &info) && info.op == ANVIL_MIR_OP_DYN_ALLOCA)
            return true;
    }
    return false;
}

static bool x86_allocation_verify(const anvil_mir_func_t *mir, const void *user)
{
    (void)user;
    return anvil_x86_verify_mir_legal(mir, NULL, 0);
}

/* What calls and atomics change: the callee's convention, and the
 * atomics' fixed EAX/ECX/EDX (gas32.c). */
static bool x86_allocation_clobbers(anvil_mir_func_t *mir, const void *user)
{
    const anvil_x86_cc_desc_t *desc = user;
    for (size_t index = 0; index < anvil_mir_num_instrs(mir); index++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, index, &info))
            return false;
        uint64_t gpr = 0, fpr = 0;
        if (info.op == ANVIL_MIR_OP_ATOMIC) {
            gpr = desc->call_gpr_clobbers;
        } else if (info.op == ANVIL_MIR_OP_CALL) {
            const anvil_x86_cc_desc_t *callee = anvil_x86_get_cc_desc(info.call_cc);
            if (!callee)
                return false;
            gpr = callee->call_gpr_clobbers;
            fpr = callee->call_fpr_clobbers;
        }
        if ((gpr && !anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_GPR, info.clobbers[ANVIL_MIR_REG_GPR] | gpr)) ||
            (fpr && !anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_FPR, info.clobbers[ANVIL_MIR_REG_FPR] | fpr)))
            return false;
    }
    return true;
}

static bool x86_allocation_printer(anvil_mir_func_t *mir, const void *user)
{
    (void)user;
    return x86_annotate_printer_registers(mir);
}

static bool x86_allocation_registerless(anvil_mir_func_t *mir, const void *user)
{
    (void)user;
    return anvil_x86_mark_registerless(mir);
}

int x86_gpr_pool(const anvil_mir_func_t *mir, const anvil_x86_cc_desc_t *desc, int *gprs)
{
    int count = 0;
    for (int r = 0; r < desc->num_alloc_gpr_regs && count < X86_GPR_POOL_MAX - 1; r++)
        gprs[count++] = desc->alloc_gpr_regs[r];
    /* EBP joins the pool (last: callee-saved, no byte form) when the
     * function keeps no frame pointer. */
    if (!x86_keeps_frame_pointer(mir))
        gprs[count++] = X86_EBP;
    return count;
}

bool anvil_x86_regalloc_mir(anvil_mir_func_t *mir)
{
    const anvil_x86_cc_desc_t *desc = anvil_x86_get_cc_desc(ANVIL_CC_CDECL);
    if (!mir || !desc)
        return false;

    /* Bytes (and booleans) only in the registers with a byte form. */
    const uint64_t byte_regs = (UINT64_C(1) << X86_EAX) | (UINT64_C(1) << X86_ECX) | (UINT64_C(1) << X86_EDX) | (UINT64_C(1) << X86_EBX);
    int gprs[X86_GPR_POOL_MAX];
    int num_gprs = x86_gpr_pool(mir, desc, gprs);
    anvil_regalloc_class_config_t configs[] = {
        {.reg_class = ANVIL_MIR_REG_GPR, .num_phys_regs = num_gprs, .phys_regs = gprs,
         .two_address = true, .narrow_bits = 8, .narrow_regs = byte_regs},
        {.reg_class = ANVIL_MIR_REG_FPR, .num_phys_regs = desc->num_alloc_fpr_regs, .phys_regs = desc->alloc_fpr_regs, .two_address = true},
    };
    /* gas32.c declares every write to them (x86_annotate_printer_registers,
     * the clobbers); its FP units use only XMM4 and the frame. */
    anvil_regalloc_class_config_t scratch_configs[] = {
        {.reg_class = ANVIL_MIR_REG_GPR, .num_phys_regs = desc->num_scratch_gpr_regs, .phys_regs = desc->scratch_gpr_regs,
         .printer_declares_writes = true},
        {.reg_class = ANVIL_MIR_REG_FPR, .num_phys_regs = desc->num_scratch_fpr_regs, .phys_regs = desc->scratch_fpr_regs,
         .printer_declares_writes = true},
    };
    anvil_mir_allocation_t allocation = {
        .verify = x86_allocation_verify,
        .annotate_clobbers = x86_allocation_clobbers,
        .address_mode = x86_address_mode,
        .annotate_printer = x86_allocation_printer,
        .mark_registerless = x86_allocation_registerless,
        .configs = configs,
        .num_configs = sizeof(configs) / sizeof(configs[0]),
        .scratch_configs = scratch_configs,
        .num_scratch_configs = sizeof(scratch_configs) / sizeof(scratch_configs[0]),
        .user = desc,
    };
    return anvil_mir_allocate(mir, &allocation);
}
