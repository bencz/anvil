#include "aapcs64.h"
#include "../regs.h"

static const int aapcs64_arg_regs_int[] = {
    ARM64_X0, ARM64_X1, ARM64_X2, ARM64_X3, ARM64_X4, ARM64_X5, ARM64_X6, ARM64_X7
};

static const int aapcs64_arg_regs_float[] = {
    ARM64_V0, ARM64_V1, ARM64_V2, ARM64_V3, ARM64_V4, ARM64_V5, ARM64_V6, ARM64_V7
};

static const int aapcs64_callee_saved[] = {
    ARM64_X19, ARM64_X20, ARM64_X21, ARM64_X22, ARM64_X23, ARM64_X24,
    ARM64_X25, ARM64_X26, ARM64_X27, ARM64_X28, ARM64_X29, ARM64_X30,
    ARM64_V8, ARM64_V9, ARM64_V10, ARM64_V11, ARM64_V12, ARM64_V13,
    ARM64_V14, ARM64_V15
};

static const int aapcs64_caller_saved[] = {
    ARM64_X0, ARM64_X1, ARM64_X2, ARM64_X3, ARM64_X4, ARM64_X5, ARM64_X6, ARM64_X7,
    ARM64_X8, ARM64_X9, ARM64_X10, ARM64_X11, ARM64_X12, ARM64_X13, ARM64_X14, ARM64_X15,
    ARM64_X16, ARM64_X17, ARM64_X18,
    ARM64_V0, ARM64_V1, ARM64_V2, ARM64_V3, ARM64_V4, ARM64_V5, ARM64_V6, ARM64_V7,
    ARM64_V16, ARM64_V17, ARM64_V18, ARM64_V19, ARM64_V20, ARM64_V21, ARM64_V22, ARM64_V23,
    ARM64_V24, ARM64_V25, ARM64_V26, ARM64_V27, ARM64_V28, ARM64_V29, ARM64_V30, ARM64_V31
};

void arm64_aapcs64_classify_argument(const AnvilABI* abi, const AnvilTargetInfo* target,
                                      AnvilType* type, int arg_index, AnvilArgInfo* out) {
    (void)abi;
    (void)target;
    
    out->size = (int)type->size;
    out->align = (int)type->align;
    
    if (anvil_type_is_float(type)) {
        if (arg_index < 8) {
            out->arg_class = ANVIL_ARG_CLASS_SSE;
            out->reg = aapcs64_arg_regs_float[arg_index];
            out->stack_offset = -1;
        } else {
            out->arg_class = ANVIL_ARG_CLASS_MEMORY;
            out->reg = -1;
        }
    } else if (anvil_type_is_integer(type) || anvil_type_is_ptr(type)) {
        if (arg_index < 8) {
            out->arg_class = ANVIL_ARG_CLASS_INTEGER;
            out->reg = aapcs64_arg_regs_int[arg_index];
            out->stack_offset = -1;
        } else {
            out->arg_class = ANVIL_ARG_CLASS_MEMORY;
            out->reg = -1;
        }
    } else if (anvil_type_is_aggregate(type)) {
        if (type->size <= 16) {
            out->arg_class = ANVIL_ARG_CLASS_INTEGER;
            if (arg_index < 8) {
                out->reg = aapcs64_arg_regs_int[arg_index];
            } else {
                out->arg_class = ANVIL_ARG_CLASS_MEMORY;
                out->reg = -1;
            }
        } else {
            out->arg_class = ANVIL_ARG_CLASS_MEMORY;
            out->reg = -1;
        }
    } else {
        out->arg_class = ANVIL_ARG_CLASS_MEMORY;
        out->reg = -1;
    }
}

void arm64_aapcs64_classify_return(const AnvilABI* abi, const AnvilTargetInfo* target,
                                    AnvilType* type, AnvilArgInfo* out) {
    (void)abi;
    (void)target;
    
    out->size = (int)type->size;
    out->align = (int)type->align;
    out->stack_offset = -1;
    
    if (anvil_type_is_void(type)) {
        out->arg_class = ANVIL_ARG_CLASS_INTEGER;
        out->reg = -1;
    } else if (anvil_type_is_float(type)) {
        out->arg_class = ANVIL_ARG_CLASS_SSE;
        out->reg = ARM64_V0;
    } else if (anvil_type_is_integer(type) || anvil_type_is_ptr(type)) {
        out->arg_class = ANVIL_ARG_CLASS_INTEGER;
        out->reg = ARM64_X0;
    } else if (anvil_type_is_aggregate(type)) {
        if (type->size <= 16) {
            out->arg_class = ANVIL_ARG_CLASS_INTEGER;
            out->reg = ARM64_X0;
        } else {
            out->arg_class = ANVIL_ARG_CLASS_MEMORY;
            out->reg = ARM64_X8;
        }
    } else {
        out->arg_class = ANVIL_ARG_CLASS_INTEGER;
        out->reg = ARM64_X0;
    }
}

void arm64_aapcs64_compute_frame_layout(const AnvilABI* abi, const AnvilTargetInfo* target,
                                         AnvilMFunc* func, AnvilFrameLayout* out) {
    (void)abi;
    (void)target;
    
    out->local_size = 0;
    out->spill_size = func->spill_slots * 8;
    out->outgoing_args_size = 0;
    out->saved_regs_size = 16;
    out->frame_pointer_offset = 0;
    
    out->total_size = out->local_size + out->spill_size + out->outgoing_args_size + out->saved_regs_size;
    out->total_size = (out->total_size + 15) & ~15;
}

const AnvilABI arm64_aapcs64_abi = {
    .name = "aapcs64",
    
    .arg_regs_int = aapcs64_arg_regs_int,
    .num_arg_regs_int = sizeof(aapcs64_arg_regs_int) / sizeof(aapcs64_arg_regs_int[0]),
    .arg_regs_float = aapcs64_arg_regs_float,
    .num_arg_regs_float = sizeof(aapcs64_arg_regs_float) / sizeof(aapcs64_arg_regs_float[0]),
    
    .ret_reg_int_lo = ARM64_X0,
    .ret_reg_int_hi = ARM64_X1,
    .ret_reg_float = ARM64_V0,
    
    .callee_saved_regs = aapcs64_callee_saved,
    .num_callee_saved = sizeof(aapcs64_callee_saved) / sizeof(aapcs64_callee_saved[0]),
    .caller_saved_regs = aapcs64_caller_saved,
    .num_caller_saved = sizeof(aapcs64_caller_saved) / sizeof(aapcs64_caller_saved[0]),
    
    .stack_alignment = 16,
    .arg_area_alignment = 8,
    .red_zone_size = 0,
    
    .args_right_to_left = false,
    .callee_cleans_stack = false,
    .return_in_memory_hidden_arg = true,
    
    .classify_argument = arm64_aapcs64_classify_argument,
    .classify_return = arm64_aapcs64_classify_return,
    .compute_frame_layout = arm64_aapcs64_compute_frame_layout,
    .format_symbol = NULL,
    .emit_call = NULL,
    .emit_string = NULL,
    .is_variadic = NULL,
    .emit_variadic_arg = NULL,
    
    .uses_underscore_prefix = false,
    .variadic_args_on_stack = false,
};
