#include "../backend.h"
#include "target.h"
#include "regs.h"
#include "emit.h"
#include "abi/elfv2.h"
#include "opt/peephole.h"
#include "../../mir/regalloc.h"
#include <string.h>
#include <stdio.h>

static const AnvilABI* ppc64_supported_abis[] = {
    &ppc64_elfv2_abi,
};

static void ppc64_lower_mir(AnvilBackend* backend, AnvilMIR* mir) {
    (void)backend;
    (void)mir;
}

static const AnvilABI* ppc64_get_abi(int os, const char* abi_name) {
    (void)os;
    (void)abi_name;
    return &ppc64_elfv2_abi;
}

static void ppc64_emit_mir_full(AnvilBackend* backend, AnvilMIR* mir, AnvilAsmBuffer* out, int os, const char* abi_name) {
    const AnvilABI* abi = ppc64_get_abi(os, abi_name);
    
    anvil_asm_append(out, "\t.abiversion 2\n");
    anvil_asm_append(out, "\t.section .text\n");
    
    for (AnvilMFunc* func = mir->first_func; func; func = func->next) {
        func->abi = abi;
        
        anvil_asm_append(out, "\t.globl %s\n", func->name);
        anvil_asm_append(out, "\t.type %s, @function\n", func->name);
        anvil_asm_append(out, "%s:\n", func->name);
        
        ppc64_emit_prologue(backend, func, out);
        
        for (size_t i = 0; i < anvil_vec_len(&func->blocks); i++) {
            AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, i);
            
            for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
                if (inst->kind == ANVIL_MIR_RET) {
                    ppc64_emit_epilogue(backend, func, out);
                }
                ppc64_emit_instruction(backend, inst, out);
            }
        }
        
        anvil_asm_append(out, "\t.size %s, .-%s\n", func->name, func->name);
        anvil_asm_append(out, "\n");
    }
    
    if (mir->string_count > 0) {
        anvil_asm_append(out, "\t.section .rodata\n");
        for (int i = 0; i < mir->string_count; i++) {
            char label[64];
            snprintf(label, sizeof(label), ".Lstr%d", mir->strings[i].id);
            if (abi->emit_string) {
                abi->emit_string(abi, label, mir->strings[i].value, out);
            } else {
                anvil_asm_append(out, "%s:\n", label);
                anvil_asm_append(out, "\t.string \"");
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

static void ppc64_select_instruction(AnvilBackend* backend, AnvilMInst* inst, AnvilVec* output) {
    (void)backend;
    (void)inst;
    (void)output;
}

static void ppc64_regalloc(AnvilBackend* backend, AnvilMFunc* func, int os, const char* abi_name) {
    const AnvilABI* abi = ppc64_get_abi(os, abi_name);
    
    static const int available_regs[] = {
        PPC64_R3, PPC64_R4, PPC64_R5, PPC64_R6, PPC64_R7, PPC64_R8, PPC64_R9, PPC64_R10,
        PPC64_R14, PPC64_R15, PPC64_R16, PPC64_R17, PPC64_R18, PPC64_R19, PPC64_R20,
        PPC64_R21, PPC64_R22, PPC64_R23, PPC64_R24, PPC64_R25, PPC64_R26, PPC64_R27,
        PPC64_R28, PPC64_R29, PPC64_R30, PPC64_R31
    };
    
    int param_prealloc[32];
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

static int ppc64_spill_cost(AnvilBackend* backend, int reg) {
    (void)backend;
    if (reg >= PPC64_R3 && reg <= PPC64_R10) return 10;
    if (reg >= PPC64_R14 && reg <= PPC64_R31) return 5;
    return 1;
}

static void ppc64_backend_peephole_optimize(AnvilBackend* backend, AnvilMFunc* func) {
    ppc64_peephole_run_all(backend, func);
}

static void ppc64_schedule_instructions(AnvilBackend* backend, AnvilMBlock* block) {
    (void)backend;
    (void)block;
}

static void ppc64_vectorize(AnvilBackend* backend, AnvilMFunc* func) {
    (void)backend;
    (void)func;
}

static const char* ppc64_reg_name_for_size(AnvilBackend* backend, int reg_id, int size_bits) {
    (void)backend;
    (void)size_bits;
    return ppc64_reg_name(reg_id);
}

static bool ppc64_immediate_fits(AnvilBackend* backend, int64_t value, AnvilMInstKind kind) {
    (void)backend;
    
    switch (kind) {
        case ANVIL_MIR_ADD:
        case ANVIL_MIR_SUB:
        case ANVIL_MIR_CMP:
            return value >= -32768 && value <= 32767;
        case ANVIL_MIR_AND:
        case ANVIL_MIR_OR:
        case ANVIL_MIR_XOR:
            return value >= 0 && value <= 65535;
        case ANVIL_MIR_MUL:
            return value >= -32768 && value <= 32767;
        default:
            return value >= -32768 && value <= 32767;
    }
}

static void ppc64_materialize_constant(AnvilBackend* backend, int64_t value, int dest_reg, AnvilVec* output) {
    (void)backend;
    (void)value;
    (void)dest_reg;
    (void)output;
}

static void ppc64_emit_label(AnvilBackend* backend, const char* label, AnvilAsmBuffer* out) {
    (void)backend;
    anvil_asm_append(out, "%s:\n", label);
}

static void ppc64_emit_data(AnvilBackend* backend, void* data, AnvilAsmBuffer* out) {
    (void)backend;
    (void)data;
    (void)out;
}

static AnvilBackend ppc64_backend = {
    .name = "ppc64",
    
    .target_info = &ppc64_target_info,
    .reg_set = &ppc64_reg_set,
    
    .default_abi = &ppc64_elfv2_abi,
    .supported_abis = ppc64_supported_abis,
    .num_supported_abis = sizeof(ppc64_supported_abis) / sizeof(ppc64_supported_abis[0]),
    
    .lower_mir = ppc64_lower_mir,
    .emit_mir = ppc64_emit_mir_full,
    .select_instruction = ppc64_select_instruction,
    .regalloc = ppc64_regalloc,
    .spill_cost = ppc64_spill_cost,
    
    .emit_prologue = ppc64_emit_prologue,
    .emit_epilogue = ppc64_emit_epilogue,
    .emit_instruction = ppc64_emit_instruction,
    .emit_label = ppc64_emit_label,
    .emit_data = ppc64_emit_data,
    
    .peephole_optimize = ppc64_backend_peephole_optimize,
    .schedule_instructions = ppc64_schedule_instructions,
    .vectorize = ppc64_vectorize,
    
    .reg_name_for_size = ppc64_reg_name_for_size,
    .immediate_fits = ppc64_immediate_fits,
    .materialize_constant = ppc64_materialize_constant,
};

AnvilBackend* anvil_create_ppc64_backend(void) {
    return &ppc64_backend;
}
