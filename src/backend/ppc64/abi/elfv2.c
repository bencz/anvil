#include "elfv2.h"
#include "../regs.h"
#include "../../../core/str.h"

static const int elfv2_arg_regs_int[] = {
    PPC64_R3, PPC64_R4, PPC64_R5, PPC64_R6,
    PPC64_R7, PPC64_R8, PPC64_R9, PPC64_R10
};

static const int elfv2_arg_regs_float[] = {
    PPC64_F1, PPC64_F2, PPC64_F3, PPC64_F4,
    PPC64_F5, PPC64_F6, PPC64_F7, PPC64_F8,
    PPC64_F9, PPC64_F10, PPC64_F11, PPC64_F12, PPC64_F13
};

static const int elfv2_callee_saved[] = {
    PPC64_R14, PPC64_R15, PPC64_R16, PPC64_R17, PPC64_R18, PPC64_R19, PPC64_R20,
    PPC64_R21, PPC64_R22, PPC64_R23, PPC64_R24, PPC64_R25, PPC64_R26, PPC64_R27,
    PPC64_R28, PPC64_R29, PPC64_R30, PPC64_R31
};

static const int elfv2_caller_saved[] = {
    PPC64_R0, PPC64_R3, PPC64_R4, PPC64_R5, PPC64_R6, PPC64_R7, PPC64_R8, PPC64_R9,
    PPC64_R10, PPC64_R11, PPC64_R12
};

static void elfv2_compute_frame_layout(const AnvilABI* abi, const AnvilTargetInfo* target,
                                        AnvilMFunc* func, AnvilFrameLayout* out) {
    (void)abi;
    (void)target;
    (void)out;
    
    int frame_size = 32;
    
    frame_size += func->spill_slots * 8;
    
    frame_size = (frame_size + 15) & ~15;
    
    func->stack_size = frame_size;
}

static void elfv2_emit_string(const AnvilABI* abi, const char* label, 
                               const char* value, AnvilAsmBuffer* out) {
    (void)abi;
    anvil_asm_append(out, "%s:\n", label);
    anvil_asm_append(out, "\t.string \"");
    for (const char* p = value; *p; p++) {
        if (*p == '\n') anvil_asm_append(out, "\\n");
        else if (*p == '\t') anvil_asm_append(out, "\\t");
        else if (*p == '\\') anvil_asm_append(out, "\\\\");
        else if (*p == '"') anvil_asm_append(out, "\\\"");
        else anvil_asm_append(out, "%c", *p);
    }
    anvil_asm_append(out, "\"\n");
}

const AnvilABI ppc64_elfv2_abi = {
    .name = "elfv2",
    
    .arg_regs_int = elfv2_arg_regs_int,
    .num_arg_regs_int = sizeof(elfv2_arg_regs_int) / sizeof(elfv2_arg_regs_int[0]),
    .arg_regs_float = elfv2_arg_regs_float,
    .num_arg_regs_float = sizeof(elfv2_arg_regs_float) / sizeof(elfv2_arg_regs_float[0]),
    
    .ret_reg_int_lo = PPC64_R3,
    .ret_reg_int_hi = PPC64_R4,
    .ret_reg_float = PPC64_F1,
    
    .callee_saved_regs = elfv2_callee_saved,
    .num_callee_saved = sizeof(elfv2_callee_saved) / sizeof(elfv2_callee_saved[0]),
    .caller_saved_regs = elfv2_caller_saved,
    .num_caller_saved = sizeof(elfv2_caller_saved) / sizeof(elfv2_caller_saved[0]),
    
    .stack_alignment = 16,
    .arg_area_alignment = 8,
    .red_zone_size = 288,
    
    .args_right_to_left = false,
    .callee_cleans_stack = false,
    .return_in_memory_hidden_arg = false,
    
    .uses_underscore_prefix = false,
    
    .variadic_args_on_stack = false,
    
    .classify_argument = NULL,
    .classify_return = NULL,
    .compute_frame_layout = elfv2_compute_frame_layout,
    .format_symbol = NULL,
    .emit_call = NULL,
    .emit_string = elfv2_emit_string,
    .is_variadic = NULL,
    .emit_variadic_arg = NULL
};
