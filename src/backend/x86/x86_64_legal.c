#include "x86_64_internal.h"
#include "x86_address.h"
#include "asm/x86_sse_vector.h"
#include "anvil/anvil_analysis.h"
#include "../common/mir_legal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool x64_legal_hook(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *instr, char *error,
                           size_t error_len, bool *decided)
{
    static const anvil_mir_legal_call_t call_rules = {.fixed_result = true, .fixed_target = true, .target_reg = -1, .fixed_args = true};
    *decided = true;
    switch (instr->op) {
    case ANVIL_MIR_OP_CALL:
        if (instr->call_cc != ANVIL_CC_SYSV && instr->call_cc != ANVIL_CC_WIN64)
            return anvil_format_failure(error, error_len, "x86-64 MIR call %zu uses an unsupported calling convention", index);
        /* SysV variadic calls carry the vector-register count passed in AL. */
        if (instr->has_imm && (instr->imm < 0 || instr->imm > 8))
            return anvil_format_failure(error, error_len, "x86-64 MIR call %zu has an invalid variadic vector-register count", index);
        return anvil_mir_legal_call(target, &call_rules, mir, index, instr, error, error_len);
    case ANVIL_MIR_OP_VA_START: {
        if (!anvil_mir_legal_pointer(target, mir, instr->def, index, error, error_len))
            return false;
        if (instr->frame_slot < 0)
            return instr->has_imm && instr->imm >= 0;
        /* SysV: the register save area. */
        anvil_mir_frame_slot_info_t slot;
        return anvil_mir_get_frame_slot_info(mir, instr->frame_slot, &slot) && slot.size_bytes >= 208 && slot.align_bytes >= 8 && instr->named_gpr <= 6 &&
               instr->named_fpr <= 8 && instr->named_stack_bytes <= INT_MAX - 16 && !(instr->named_stack_bytes % 8);
    }
    case ANVIL_MIR_OP_CALL_RESULT: {
        /* Further result registers are fixed vregs defined by the call. */
        const anvil_mir_vreg_info_t *def = anvil_mir_get_vreg_info(mir, instr->def);
        return (def && def->has_fixed_reg) ||
               anvil_format_failure(error, error_len, "x86-64 MIR call result %zu must be fixed to an ABI result register", index);
    }
    default:
        *decided = false;
        return true;
    }
}

bool anvil_x86_64_verify_mir_legal(const anvil_mir_func_t *mir, char *error, size_t error_len)
{
    static const anvil_mir_legal_target_t target = {
        .name = "x86-64",
        .widths = {[ANVIL_MIR_REG_GPR] = ANVIL_MIR_WIDTH(8) | ANVIL_MIR_WIDTH(16) | ANVIL_MIR_WIDTH(32) | ANVIL_MIR_WIDTH(64),
                   [ANVIL_MIR_REG_FPR] = ANVIL_MIR_WIDTH(32) | ANVIL_MIR_WIDTH(64) | ANVIL_MIR_WIDTH(128)},
        .num_regs = {[ANVIL_MIR_REG_GPR] = X64_NUM_GPR, [ANVIL_MIR_REG_FPR] = X64_NUM_FPR},
        .pointer_bits = 64,
        .fp_vector_bits = 128,
        .stack_arg_align = 8,
        /* CMOVcc: general registers (FP selects lower to branches). */
        .select_classes = 1u << ANVIL_MIR_REG_GPR,
        .scalar_isa = x86_sse_isa,
        .vector_isa = x86_sse_isa,
        .hook = x64_legal_hook,
    };
    anvil_mir_legal_target_t sized = target;
    sized.scalar_isa_count = sized.vector_isa_count = x86_sse_isa_count;
    return anvil_mir_verify_legal(&sized, mir, error, error_len);
}

static bool x64_allocation_verify(const anvil_mir_func_t *mir, const void *user)
{
    (void)user;
    return anvil_x86_64_verify_mir_legal(mir, NULL, 0);
}

/* What calls change (the callee's convention), and the registers atomics
 * and the SysV register save of VA_START use (gas64.c). */
static bool x64_allocation_clobbers(anvil_mir_func_t *mir, const void *user)
{
    (void)user;
    for (size_t index = 0; index < anvil_mir_num_instrs(mir); index++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, index, &info))
            return false;
        uint64_t gpr = 0, fpr = 0;
        if (info.op == ANVIL_MIR_OP_ATOMIC)
            gpr = (UINT64_C(1) << X64_RAX) | (UINT64_C(1) << X64_RCX) | (UINT64_C(1) << X64_RDX) | (UINT64_C(1) << X64_R11);
        else if (info.op == ANVIL_MIR_OP_VA_START && info.frame_slot >= 0)
            gpr = UINT64_C(1) << X64_R11;
        else if (info.op == ANVIL_MIR_OP_CALL) {
            const anvil_x64_abi_desc_t *callee = anvil_x64_get_abi_desc(info.call_cc == ANVIL_CC_WIN64 ? ANVIL_ABI_WIN64 : ANVIL_ABI_SYSV);
            gpr = callee->call_gpr_clobbers;
            fpr = callee->call_fpr_clobbers;
        }
        if ((gpr && !anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_GPR, info.clobbers[ANVIL_MIR_REG_GPR] | gpr)) ||
            (fpr && !anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_FPR, info.clobbers[ANVIL_MIR_REG_FPR] | fpr)))
            return false;
    }
    return true;
}

static bool x64_allocation_printer(anvil_mir_func_t *mir, const void *user)
{
    return x64_annotate_printer_scratch(mir, user);
}

static bool x64_allocation_registerless(anvil_mir_func_t *mir, const void *user)
{
    (void)user;
    return anvil_x64_mark_registerless(mir);
}

bool anvil_x86_64_regalloc_mir_abi(anvil_mir_func_t *mir, anvil_abi_t abi)
{
    const anvil_x64_abi_desc_t *desc = anvil_x64_get_abi_desc(abi);
    if (!desc)
        return false;
    anvil_regalloc_class_config_t configs[] = {
        {.reg_class = ANVIL_MIR_REG_GPR, .num_phys_regs = desc->num_alloc_gpr_regs, .phys_regs = desc->alloc_gpr_regs, .two_address = true},
        {.reg_class = ANVIL_MIR_REG_FPR, .num_phys_regs = desc->num_alloc_fpr_regs, .phys_regs = desc->alloc_fpr_regs, .two_address = true},
    };
    /* gas64.c declares every write to them (x64_annotate_printer_scratch,
     * the clobbers). */
    anvil_regalloc_class_config_t scratch_configs[] = {
        {.reg_class = ANVIL_MIR_REG_GPR, .num_phys_regs = desc->num_scratch_gpr_regs, .phys_regs = desc->scratch_gpr_regs,
         .printer_declares_writes = true},
        {.reg_class = ANVIL_MIR_REG_FPR, .num_phys_regs = desc->num_scratch_fpr_regs, .phys_regs = desc->scratch_fpr_regs,
         .printer_declares_writes = true},
    };
    anvil_mir_allocation_t allocation = {
        .verify = x64_allocation_verify,
        .annotate_clobbers = x64_allocation_clobbers,
        .address_mode = x86_address_mode,
        .annotate_printer = x64_allocation_printer,
        .mark_registerless = x64_allocation_registerless,
        .configs = configs,
        .num_configs = sizeof(configs) / sizeof(configs[0]),
        .scratch_configs = scratch_configs,
        .num_scratch_configs = sizeof(scratch_configs) / sizeof(scratch_configs[0]),
        .user = desc,
    };
    return anvil_mir_allocate(mir, &allocation);
}

bool anvil_x86_64_regalloc_mir(anvil_mir_func_t *mir)
{
    return anvil_x86_64_regalloc_mir_abi(mir, ANVIL_ABI_SYSV);
}

unsigned x64_vector_operation_cost(anvil_backend_t *be, anvil_op_t operation, anvil_type_t *type, anvil_vector_shape_t shape)
{
    return anvil_vector_isa_cost(x86_sse_isa, x86_sse_isa_count, 16, be->ctx, operation, type, shape);
}
