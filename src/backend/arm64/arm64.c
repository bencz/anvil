#include "../backend.h"
#include "target.h"
#include "regs.h"
#include "emit.h"
#include "abi/aapcs64.h"
#include "abi/apple.h"
#include "opt/peephole.h"
#include "isel/isel.h"
#include "../../mir/regalloc.h"
#include <string.h>
#include <stdio.h>

static const AnvilABI* arm64_supported_abis[] = {
    &arm64_aapcs64_abi,
    &arm64_apple_abi,
};

static void arm64_lower_mir(AnvilBackend* backend, AnvilMIR* mir) {
    (void)backend;
    (void)mir;
}

static const AnvilABI* arm64_get_abi(int os, const char* abi_name) {
    if (abi_name) {
        if (strcmp(abi_name, "apple") == 0) return &arm64_apple_abi;
        if (strcmp(abi_name, "aapcs64") == 0) return &arm64_aapcs64_abi;
    }
    if (os == ANVIL_OS_MACOS) return &arm64_apple_abi;
    return &arm64_aapcs64_abi;
}

static void arm64_emit_mir_full(AnvilBackend* backend, AnvilMIR* mir, AnvilAsmBuffer* out, int os, const char* abi_name) {
    const AnvilABI* abi = arm64_get_abi(os, abi_name);
    const char* prefix = abi->uses_underscore_prefix ? "_" : "";
    
    anvil_asm_append(out, "\t.text\n");
    
    for (AnvilMFunc* func = mir->first_func; func; func = func->next) {
        func->abi = abi;
        
        if (abi->uses_underscore_prefix) {
            anvil_asm_append(out, "\t.globl %s%s\n", prefix, func->name);
            anvil_asm_append(out, "\t.p2align 2\n");
            anvil_asm_append(out, "%s%s:\n", prefix, func->name);
        } else {
            anvil_asm_append(out, "\t.globl %s\n", func->name);
            anvil_asm_append(out, "\t.type %s, %%function\n", func->name);
            anvil_asm_append(out, "%s:\n", func->name);
        }
        
        if (func->needs_frame) {
            arm64_emit_prologue(backend, func, out);
        }
        
        for (size_t i = 0; i < anvil_vec_len(&func->blocks); i++) {
            AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, i);
            
            for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
                if (inst->kind == ANVIL_MIR_RET && func->needs_frame) {
                    arm64_emit_epilogue(backend, func, out);
                }
                arm64_emit_instruction(backend, inst, out);
            }
        }
        
        if (!abi->uses_underscore_prefix) {
            anvil_asm_append(out, "\t.size %s, .-%s\n", func->name, func->name);
        }
        anvil_asm_append(out, "\n");
    }
    
    if (mir->string_count > 0) {
        if (abi->uses_underscore_prefix) {
            anvil_asm_append(out, "\t.section __DATA,__cstring\n");
        } else {
            anvil_asm_append(out, "\t.section .rodata\n");
        }
        for (int i = 0; i < mir->string_count; i++) {
            char label[64];
            snprintf(label, sizeof(label), ".Lstr%d", mir->strings[i].id);
            if (abi->emit_string) {
                abi->emit_string(abi, label, mir->strings[i].value, out);
            } else {
                anvil_asm_append(out, "%s:\n", label);
                anvil_asm_append(out, "\t.asciz \"");
                for (const char* p = mir->strings[i].value; *p; p++) {
                    if (*p == '\n') anvil_asm_append(out, "\\n");
                    else if (*p == '\t') anvil_asm_append(out, "\\t");
                    else if (*p == '\\') anvil_asm_append(out, "\\\\");
                    else if (*p == '"') anvil_asm_append(out, "\\\"");
                    else anvil_asm_append(out, "%c", *p);
                }
                anvil_asm_append(out, "\"\n");
            }
        }
    }
}

static void arm64_regalloc(AnvilBackend* backend, AnvilMFunc* func, int os, const char* abi_name) {
    const AnvilABI* abi = arm64_get_abi(os, abi_name);
    
    static const int available_regs[] = {
        ARM64_X0, ARM64_X1, ARM64_X2, ARM64_X3, ARM64_X4, ARM64_X5, ARM64_X6, ARM64_X7,
        ARM64_X8, ARM64_X9, ARM64_X10, ARM64_X11, ARM64_X12, ARM64_X13, ARM64_X14, ARM64_X15,
        ARM64_X16, ARM64_X17,
        ARM64_X19, ARM64_X20, ARM64_X21, ARM64_X22, ARM64_X23, ARM64_X24,
        ARM64_X25, ARM64_X26, ARM64_X27, ARM64_X28
    };
    
    static const int available_fp_regs[] = {
        ARM64_V0, ARM64_V1, ARM64_V2, ARM64_V3, ARM64_V4, ARM64_V5, ARM64_V6, ARM64_V7,
        ARM64_V8, ARM64_V9, ARM64_V10, ARM64_V11, ARM64_V12, ARM64_V13, ARM64_V14, ARM64_V15,
        ARM64_V16, ARM64_V17, ARM64_V18, ARM64_V19, ARM64_V20, ARM64_V21, ARM64_V22, ARM64_V23,
        ARM64_V24, ARM64_V25, ARM64_V26, ARM64_V27, ARM64_V28, ARM64_V29, ARM64_V30, ARM64_V31
    };
    
    int param_prealloc[32];
    int num_prealloc = 0;
    int fp_param_prealloc[32];
    int num_fp_prealloc = 0;
    
    int int_idx = 0;
    int fp_idx = 0;
    for (int i = 0; i < func->num_params; i++) {
        AnvilMOperand* param = (AnvilMOperand*)anvil_vec_get(&func->params, i);
        if (param->is_fp) {
            if (fp_idx < abi->num_arg_regs_float) {
                fp_param_prealloc[num_fp_prealloc++] = param->vreg.id;
                fp_param_prealloc[num_fp_prealloc++] = abi->arg_regs_float[fp_idx];
            }
            fp_idx++;
        } else {
            if (int_idx < abi->num_arg_regs_int) {
                param_prealloc[num_prealloc++] = param->vreg.id;
                param_prealloc[num_prealloc++] = abi->arg_regs_int[int_idx];
            }
            int_idx++;
        }
    }
    
    AnvilRegAllocConfig config = {
        .available_regs = available_regs,
        .num_available_regs = sizeof(available_regs) / sizeof(available_regs[0]),
        .available_fp_regs = available_fp_regs,
        .num_available_fp_regs = sizeof(available_fp_regs) / sizeof(available_fp_regs[0]),
        .callee_saved = abi->callee_saved_regs,
        .num_callee_saved = abi->num_callee_saved,
        .stack_slot_size = 8,
        .prealloc = param_prealloc,
        .num_prealloc = num_prealloc / 2,
        .prealloc_fp = fp_param_prealloc,
        .num_prealloc_fp = num_fp_prealloc / 2,
    };
    
    AnvilRegAllocResult* result = anvil_regalloc_linear_scan(func, &config);
    anvil_regalloc_apply(func, result, &config);
    anvil_regalloc_result_free(result);
    
    (void)backend;
}

static int arm64_spill_cost(AnvilBackend* backend, int reg) {
    (void)backend;
    if (reg >= ARM64_X0 && reg <= ARM64_X7) return 10;
    if (reg >= ARM64_X19 && reg <= ARM64_X28) return 5;
    return 1;
}

static void arm64_backend_peephole_optimize(AnvilBackend* backend, AnvilMFunc* func) {
    arm64_peephole_run_all(backend, func);
}

static void arm64_schedule_instructions(AnvilBackend* backend, AnvilMBlock* block) {
    (void)backend;
    (void)block;
}

static void arm64_vectorize(AnvilBackend* backend, AnvilMFunc* func) {
    (void)backend;
    (void)func;
}

static const char* arm64_reg_name_for_size(AnvilBackend* backend, int reg_id, int size_bits) {
    (void)backend;
    return arm64_reg_name(reg_id, size_bits);
}

static bool arm64_immediate_fits(AnvilBackend* backend, int64_t value, AnvilMInstKind kind) {
    (void)backend;
    (void)kind;
    return value >= 0 && value <= 4095;
}

static void arm64_materialize_constant(AnvilBackend* backend, int64_t value, int dest_reg, AnvilVec* output) {
    (void)backend;
    (void)value;
    (void)dest_reg;
    (void)output;
}

static AnvilBackend arm64_backend = {
    .name = "arm64",
    
    .target_info = &arm64_target_info,
    .reg_set = &arm64_reg_set,
    
    .default_abi = &arm64_aapcs64_abi,
    .supported_abis = arm64_supported_abis,
    .num_supported_abis = sizeof(arm64_supported_abis) / sizeof(arm64_supported_abis[0]),
    
    .lower_mir = arm64_lower_mir,
    .emit_mir = arm64_emit_mir_full,
    .regalloc = arm64_regalloc,
    .spill_cost = arm64_spill_cost,
    
    .emit_prologue = arm64_emit_prologue,
    .emit_epilogue = arm64_emit_epilogue,
    .emit_instruction = arm64_emit_instruction,
    .emit_label = arm64_emit_label,
    .emit_data = arm64_emit_data,
    
    .peephole_optimize = arm64_backend_peephole_optimize,
    .schedule_instructions = arm64_schedule_instructions,
    .vectorize = arm64_vectorize,
    .isel = arm64_isel_run,
    
    .reg_name_for_size = arm64_reg_name_for_size,
    .immediate_fits = arm64_immediate_fits,
    .materialize_constant = arm64_materialize_constant,
};

AnvilBackend* anvil_create_arm64_backend(void) {
    return &arm64_backend;
}
