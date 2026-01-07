#include "../backend.h"
#include "target.h"
#include "regs.h"
#include "emit.h"
#include "abi/sysv.h"
#include "abi/win64.h"
#include "opt/peephole.h"
#include "../../mir/regalloc.h"
#include <string.h>

static const AnvilABI* x86_64_supported_abis[] = {
    &x86_64_sysv_abi,
    &x86_64_win64_abi,
};

static void x86_64_lower_mir(AnvilBackend* backend, AnvilMIR* mir) {
    (void)backend;
    (void)mir;
}

static const AnvilABI* x86_64_get_abi(int os, const char* abi_name) {
    if (abi_name) {
        if (strcmp(abi_name, "win64") == 0) return &x86_64_win64_abi;
        if (strcmp(abi_name, "sysv") == 0) return &x86_64_sysv_abi;
    }
    if (os == ANVIL_OS_WINDOWS) return &x86_64_win64_abi;
    return &x86_64_sysv_abi;
}

static void x86_64_emit_mir_full(AnvilBackend* backend, AnvilMIR* mir, AnvilAsmBuffer* out, int os, const char* abi_name) {
    const AnvilABI* abi = x86_64_get_abi(os, abi_name);
    const char* prefix = abi->uses_underscore_prefix ? "_" : "";
    
    anvil_asm_append(out, "\t.text\n");
    
    for (AnvilMFunc* func = mir->first_func; func; func = func->next) {
        func->abi = abi;
        
        if (abi->uses_underscore_prefix) {
            anvil_asm_append(out, "\t.globl %s%s\n", prefix, func->name);
            anvil_asm_append(out, "%s%s:\n", prefix, func->name);
        } else {
            anvil_asm_append(out, "\t.globl %s\n", func->name);
            anvil_asm_append(out, "\t.type %s, @function\n", func->name);
            anvil_asm_append(out, "%s:\n", func->name);
        }
        
        x86_64_emit_prologue(backend, func, out);
        
        for (size_t i = 0; i < anvil_vec_len(&func->blocks); i++) {
            AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, i);
            
            for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
                if (inst->kind == ANVIL_MIR_RET) {
                    x86_64_emit_epilogue(backend, func, out);
                }
                x86_64_emit_instruction(backend, inst, out);
            }
        }
        
        if (!abi->uses_underscore_prefix) {
            anvil_asm_append(out, "\t.size %s, .-%s\n", func->name, func->name);
        }
        anvil_asm_append(out, "\n");
    }
}

static void x86_64_select_instruction(AnvilBackend* backend, AnvilMInst* inst, AnvilVec* output) {
    (void)backend;
    (void)inst;
    (void)output;
}

static void x86_64_regalloc(AnvilBackend* backend, AnvilMFunc* func, int os, const char* abi_name) {
    const AnvilABI* abi = x86_64_get_abi(os, abi_name);
    
    static const int available_regs[] = {
        X86_64_RAX, X86_64_RCX, X86_64_RDX, X86_64_RSI, X86_64_RDI,
        X86_64_R8, X86_64_R9, X86_64_R10, X86_64_R11,
        X86_64_RBX, X86_64_R12, X86_64_R13, X86_64_R14, X86_64_R15
    };
    
    int param_prealloc[16];
    int num_prealloc = 0;
    for (int i = 0; i < func->num_params && i < abi->num_arg_regs_int; i++) {
        param_prealloc[num_prealloc++] = i + 1;
        param_prealloc[num_prealloc++] = abi->arg_regs_int[i];
    }
    
    AnvilRegAllocConfig config = {
        .available_regs = available_regs,
        .num_available_regs = sizeof(available_regs) / sizeof(available_regs[0]),
        .callee_saved = abi->callee_saved_regs,
        .num_callee_saved = abi->num_callee_saved,
        .stack_slot_size = 8,
        .prealloc = param_prealloc,
        .num_prealloc = num_prealloc / 2,
    };
    
    AnvilRegAllocResult* result = anvil_regalloc_linear_scan(func, &config);
    anvil_regalloc_apply(func, result, &config);
    anvil_regalloc_result_free(result);
    
    (void)backend;
}

static int x86_64_spill_cost(AnvilBackend* backend, int reg) {
    (void)backend;
    if (reg == X86_64_RAX || reg == X86_64_RDX) return 10;
    if (reg >= X86_64_RBX && reg <= X86_64_R15) return 5;
    return 1;
}

static void x86_64_backend_peephole_optimize(AnvilBackend* backend, AnvilMFunc* func) {
    x86_64_peephole_run_all(backend, func);
}

static void x86_64_schedule_instructions(AnvilBackend* backend, AnvilMBlock* block) {
    (void)backend;
    (void)block;
}

static void x86_64_vectorize(AnvilBackend* backend, AnvilMFunc* func) {
    (void)backend;
    (void)func;
}

static const char* x86_64_reg_name_for_size(AnvilBackend* backend, int reg_id, int size_bits) {
    (void)backend;
    return x86_64_reg_name(reg_id, size_bits);
}

static bool x86_64_immediate_fits(AnvilBackend* backend, int64_t value, AnvilMInstKind kind) {
    (void)backend;
    (void)kind;
    return value >= -2147483648LL && value <= 2147483647LL;
}

static void x86_64_materialize_constant(AnvilBackend* backend, int64_t value, int dest_reg, AnvilVec* output) {
    (void)backend;
    (void)value;
    (void)dest_reg;
    (void)output;
}

static AnvilBackend x86_64_backend = {
    .name = "x86_64",
    
    .target_info = &x86_64_target_info,
    .reg_set = &x86_64_reg_set,
    
    .default_abi = &x86_64_sysv_abi,
    .supported_abis = x86_64_supported_abis,
    .num_supported_abis = sizeof(x86_64_supported_abis) / sizeof(x86_64_supported_abis[0]),
    
    .lower_mir = x86_64_lower_mir,
    .emit_mir = x86_64_emit_mir_full,
    .select_instruction = x86_64_select_instruction,
    .regalloc = x86_64_regalloc,
    .spill_cost = x86_64_spill_cost,
    
    .emit_prologue = x86_64_emit_prologue,
    .emit_epilogue = x86_64_emit_epilogue,
    .emit_instruction = x86_64_emit_instruction,
    .emit_label = x86_64_emit_label,
    .emit_data = x86_64_emit_data,
    
    .peephole_optimize = x86_64_backend_peephole_optimize,
    .schedule_instructions = x86_64_schedule_instructions,
    .vectorize = x86_64_vectorize,
    
    .reg_name_for_size = x86_64_reg_name_for_size,
    .immediate_fits = x86_64_immediate_fits,
    .materialize_constant = x86_64_materialize_constant,
};

AnvilBackend* anvil_create_x86_64_backend(void) {
    return &x86_64_backend;
}
