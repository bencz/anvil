#include "sysv.h"
#include "../regs.h"

static const int sysv_arg_regs_int[] = {
    X86_64_RDI, X86_64_RSI, X86_64_RDX, X86_64_RCX, X86_64_R8, X86_64_R9
};

static const int sysv_arg_regs_float[] = {
    X86_64_XMM0, X86_64_XMM1, X86_64_XMM2, X86_64_XMM3,
    X86_64_XMM4, X86_64_XMM5, X86_64_XMM6, X86_64_XMM7
};

static const int sysv_callee_saved[] = {
    X86_64_RBX, X86_64_RBP, X86_64_R12, X86_64_R13, X86_64_R14, X86_64_R15
};

static const int sysv_caller_saved[] = {
    X86_64_RAX, X86_64_RCX, X86_64_RDX, X86_64_RSI, X86_64_RDI,
    X86_64_R8, X86_64_R9, X86_64_R10, X86_64_R11,
    X86_64_XMM0, X86_64_XMM1, X86_64_XMM2, X86_64_XMM3,
    X86_64_XMM4, X86_64_XMM5, X86_64_XMM6, X86_64_XMM7,
    X86_64_XMM8, X86_64_XMM9, X86_64_XMM10, X86_64_XMM11,
    X86_64_XMM12, X86_64_XMM13, X86_64_XMM14, X86_64_XMM15
};

void x86_64_sysv_classify_argument(const AnvilABI* abi, const AnvilTargetInfo* target,
                                    AnvilType* type, int arg_index, AnvilArgInfo* out) {
    (void)abi;
    (void)target;
    
    out->size = (int)type->size;
    out->align = (int)type->align;
    
    if (anvil_type_is_float(type)) {
        if (arg_index < 8) {
            out->arg_class = ANVIL_ARG_CLASS_SSE;
            out->reg = sysv_arg_regs_float[arg_index];
            out->stack_offset = -1;
        } else {
            out->arg_class = ANVIL_ARG_CLASS_MEMORY;
            out->reg = -1;
        }
    } else if (anvil_type_is_integer(type) || anvil_type_is_ptr(type)) {
        if (arg_index < 6) {
            out->arg_class = ANVIL_ARG_CLASS_INTEGER;
            out->reg = sysv_arg_regs_int[arg_index];
            out->stack_offset = -1;
        } else {
            out->arg_class = ANVIL_ARG_CLASS_MEMORY;
            out->reg = -1;
        }
    } else if (anvil_type_is_aggregate(type)) {
        if (type->size <= 16) {
            out->arg_class = ANVIL_ARG_CLASS_INTEGER;
            if (arg_index < 6) {
                out->reg = sysv_arg_regs_int[arg_index];
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

void x86_64_sysv_classify_return(const AnvilABI* abi, const AnvilTargetInfo* target,
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
        out->reg = X86_64_XMM0;
    } else if (anvil_type_is_integer(type) || anvil_type_is_ptr(type)) {
        out->arg_class = ANVIL_ARG_CLASS_INTEGER;
        out->reg = X86_64_RAX;
    } else if (anvil_type_is_aggregate(type)) {
        if (type->size <= 16) {
            out->arg_class = ANVIL_ARG_CLASS_INTEGER;
            out->reg = X86_64_RAX;
        } else {
            out->arg_class = ANVIL_ARG_CLASS_MEMORY;
            out->reg = X86_64_RDI;
        }
    } else {
        out->arg_class = ANVIL_ARG_CLASS_INTEGER;
        out->reg = X86_64_RAX;
    }
}

void x86_64_sysv_compute_frame_layout(const AnvilABI* abi, const AnvilTargetInfo* target,
                                       AnvilMFunc* func, AnvilFrameLayout* out) {
    (void)abi;
    (void)target;
    
    out->local_size = 0;
    out->spill_size = func->spill_slots * 8;
    out->outgoing_args_size = 0;
    out->saved_regs_size = 0;
    out->frame_pointer_offset = 0;
    
    out->total_size = out->local_size + out->spill_size + out->outgoing_args_size + out->saved_regs_size;
    out->total_size = (out->total_size + 15) & ~15;
}

const AnvilABI x86_64_sysv_abi = {
    .name = "sysv_amd64",
    
    .arg_regs_int = sysv_arg_regs_int,
    .num_arg_regs_int = sizeof(sysv_arg_regs_int) / sizeof(sysv_arg_regs_int[0]),
    .arg_regs_float = sysv_arg_regs_float,
    .num_arg_regs_float = sizeof(sysv_arg_regs_float) / sizeof(sysv_arg_regs_float[0]),
    
    .ret_reg_int_lo = X86_64_RAX,
    .ret_reg_int_hi = X86_64_RDX,
    .ret_reg_float = X86_64_XMM0,
    
    .callee_saved_regs = sysv_callee_saved,
    .num_callee_saved = sizeof(sysv_callee_saved) / sizeof(sysv_callee_saved[0]),
    .caller_saved_regs = sysv_caller_saved,
    .num_caller_saved = sizeof(sysv_caller_saved) / sizeof(sysv_caller_saved[0]),
    
    .stack_alignment = 16,
    .arg_area_alignment = 8,
    .red_zone_size = 128,
    
    .args_right_to_left = false,
    .callee_cleans_stack = false,
    .return_in_memory_hidden_arg = true,
    
    .classify_argument = x86_64_sysv_classify_argument,
    .classify_return = x86_64_sysv_classify_return,
    .compute_frame_layout = x86_64_sysv_compute_frame_layout,
    .format_symbol = NULL,
    .emit_call = NULL,
    .emit_string = NULL,
    .is_variadic = NULL,
    .emit_variadic_arg = NULL,
    
    .uses_underscore_prefix = false,
    .variadic_args_on_stack = false,
};
