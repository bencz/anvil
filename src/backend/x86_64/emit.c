#include "emit.h"
#include "../../core/str.h"
#include <stdio.h>

static const char* size_suffix(int size) {
    switch (size) {
        case 1: return "b";
        case 2: return "w";
        case 4: return "l";
        case 8: return "q";
        default: return "q";
    }
}

static const char* xmm_reg_name(int id) {
    static const char* names[] = {
        "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
        "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
    };
    if (id >= X86_64_XMM0 && id <= X86_64_XMM15) {
        return names[id - X86_64_XMM0];
    }
    if (id >= 0 && id < 16) {
        return names[id];
    }
    return "xmm0";
}

static void emit_operand(AnvilBackend* backend, AnvilMOperand* op, AnvilAsmBuffer* out) {
    switch (op->kind) {
        case ANVIL_MOP_PREG:
            if (op->is_fp) {
                anvil_asm_append(out, "%%%s", xmm_reg_name(op->preg.id));
            } else {
                anvil_asm_append(out, "%%%s", x86_64_reg_name(op->preg.id, op->size * 8));
            }
            break;
        case ANVIL_MOP_VREG:
            if (op->is_fp) {
                anvil_asm_append(out, "%%xmm_v%d", op->vreg.id);
            } else {
                anvil_asm_append(out, "%%v%d", op->vreg.id);
            }
            break;
        case ANVIL_MOP_IMM:
            anvil_asm_append(out, "$%lld", (long long)op->imm.value);
            break;
        case ANVIL_MOP_MEM:
            if (op->mem.disp != 0) {
                anvil_asm_append(out, "%lld", (long long)op->mem.disp);
            }
            anvil_asm_append(out, "(");
            if (op->mem.base_reg >= 0) {
                if (op->mem.base_is_vreg) {
                    anvil_asm_append(out, "%%v%d", op->mem.base_reg);
                } else {
                    anvil_asm_append(out, "%%%s", x86_64_reg_name(op->mem.base_reg, 64));
                }
            }
            if (op->mem.index_reg >= 0) {
                anvil_asm_append(out, ",");
                if (op->mem.index_is_vreg) {
                    anvil_asm_append(out, "%%v%d", op->mem.index_reg);
                } else {
                    anvil_asm_append(out, "%%%s", x86_64_reg_name(op->mem.index_reg, 64));
                }
                if (op->mem.scale > 1) {
                    anvil_asm_append(out, ",%d", op->mem.scale);
                }
            }
            anvil_asm_append(out, ")");
            break;
        case ANVIL_MOP_LABEL:
            anvil_asm_append(out, "%s", op->label.name);
            break;
        case ANVIL_MOP_FUNC:
            anvil_asm_append(out, "%s", op->func.name);
            break;
        case ANVIL_MOP_GLOBAL:
            anvil_asm_append(out, "%s(%%rip)", op->global.name);
            break;
        default:
            break;
    }
    (void)backend;
}

static const char* cc_suffix(AnvilCondCode cc) {
    switch (cc) {
        case ANVIL_CC_EQ: return "e";
        case ANVIL_CC_NE: return "ne";
        case ANVIL_CC_LT: return "l";
        case ANVIL_CC_LE: return "le";
        case ANVIL_CC_GT: return "g";
        case ANVIL_CC_GE: return "ge";
        case ANVIL_CC_ULT: return "b";
        case ANVIL_CC_ULE: return "be";
        case ANVIL_CC_UGT: return "a";
        case ANVIL_CC_UGE: return "ae";
        case ANVIL_CC_S: return "s";
        case ANVIL_CC_NS: return "ns";
        case ANVIL_CC_O: return "o";
        case ANVIL_CC_NO: return "no";
        default: return "e";
    }
}

void x86_64_emit_instruction(AnvilBackend* backend, AnvilMInst* inst, AnvilAsmBuffer* out) {
    switch (inst->kind) {
        case ANVIL_MIR_NOP:
            anvil_asm_append(out, "\tnop\n");
            break;
            
        case ANVIL_MIR_MOV:
            anvil_asm_append(out, "\tmov%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_MOVSS:
            anvil_asm_append(out, "\tmovss ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_MOVSD:
            anvil_asm_append(out, "\tmovsd ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_MOV_IMM:
            anvil_asm_append(out, "\tmov%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_LOAD:
            anvil_asm_append(out, "\tmov%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_STORE:
            anvil_asm_append(out, "\tmov%s ", size_suffix(inst->operands[1].size));
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_LEA:
            anvil_asm_append(out, "\tleaq ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_ADD:
            anvil_asm_append(out, "\tadd%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_SUB:
            anvil_asm_append(out, "\tsub%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_MUL:
        case ANVIL_MIR_IMUL:
            anvil_asm_append(out, "\timul%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_DIV:
        case ANVIL_MIR_IDIV:
            anvil_asm_append(out, "\tidiv%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_NEG:
            anvil_asm_append(out, "\tneg%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_AND:
            anvil_asm_append(out, "\tand%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_OR:
            anvil_asm_append(out, "\tor%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_XOR:
            anvil_asm_append(out, "\txor%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_NOT:
            anvil_asm_append(out, "\tnot%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_SHL:
            anvil_asm_append(out, "\tshl%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_SHR:
            anvil_asm_append(out, "\tshr%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_SAR:
            anvil_asm_append(out, "\tsar%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_CMP:
            anvil_asm_append(out, "\tcmp%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_TEST:
            anvil_asm_append(out, "\ttest%s ", size_suffix(inst->operands[0].size));
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_SETCC:
            anvil_asm_append(out, "\tset%s ", cc_suffix(inst->cc));
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_JMP:
            anvil_asm_append(out, "\tjmp ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_JCC:
            anvil_asm_append(out, "\tj%s ", cc_suffix(inst->cc));
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_RET:
            anvil_asm_append(out, "\tret\n");
            break;
            
        case ANVIL_MIR_CALL:
            if (inst->operands[0].kind == ANVIL_MOP_FUNC) {
                const char* prefix = "";
                if (inst->func && inst->func->abi && inst->func->abi->uses_underscore_prefix) {
                    prefix = "_";
                }
                anvil_asm_append(out, "\tcall %s%s\n", prefix, inst->operands[0].func.name);
            } else {
                anvil_asm_append(out, "\tcall ");
                emit_operand(backend, &inst->operands[0], out);
                anvil_asm_append(out, "\n");
            }
            break;
            
        case ANVIL_MIR_CALL_INDIRECT:
            anvil_asm_append(out, "\tcall *");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_CALL_PLT:
            if (inst->operands[0].kind == ANVIL_MOP_FUNC) {
                anvil_asm_append(out, "\tcall %s@PLT\n", inst->operands[0].func.name);
            }
            break;
            
        case ANVIL_MIR_CALL_GOT:
            if (inst->operands[0].kind == ANVIL_MOP_FUNC) {
                anvil_asm_append(out, "\tcall *%s@GOTPCREL(%%rip)\n", inst->operands[0].func.name);
            }
            break;
            
        case ANVIL_MIR_TAIL_CALL:
            if (inst->operands[0].kind == ANVIL_MOP_FUNC) {
                const char* prefix = "";
                if (inst->func && inst->func->abi && inst->func->abi->uses_underscore_prefix) {
                    prefix = "_";
                }
                anvil_asm_append(out, "\tjmp %s%s\n", prefix, inst->operands[0].func.name);
            } else {
                anvil_asm_append(out, "\tjmp *");
                emit_operand(backend, &inst->operands[0], out);
                anvil_asm_append(out, "\n");
            }
            break;
            
        case ANVIL_MIR_PUSH:
            anvil_asm_append(out, "\tpushq ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_POP:
            anvil_asm_append(out, "\tpopq ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_ZEXT:
            anvil_asm_append(out, "\tmovzx ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_SEXT:
            anvil_asm_append(out, "\tmovsx ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_LABEL:
            anvil_asm_append(out, "%s:\n", inst->operands[0].label.name);
            break;
            
        case ANVIL_MIR_COMMENT:
            if (inst->comment) {
                anvil_asm_append(out, "\t# %s\n", inst->comment);
            }
            break;
            
        case ANVIL_MIR_FADD:
            anvil_asm_append(out, "\tadd%s ", inst->operands[0].size == 4 ? "ss" : "sd");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_FSUB:
            anvil_asm_append(out, "\tsub%s ", inst->operands[0].size == 4 ? "ss" : "sd");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_FMUL:
            anvil_asm_append(out, "\tmul%s ", inst->operands[0].size == 4 ? "ss" : "sd");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_FDIV:
            anvil_asm_append(out, "\tdiv%s ", inst->operands[0].size == 4 ? "ss" : "sd");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_FNEG: {
            const char* suffix = inst->operands[0].size == 4 ? "ss" : "sd";
            anvil_asm_append(out, "\txorp%s .LC_signbit_%s(%%rip), ", suffix, suffix);
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
        }
            
        case ANVIL_MIR_FCMP:
            anvil_asm_append(out, "\tucomi%s ", inst->operands[0].size == 4 ? "ss" : "sd");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_CVTSI2SS:
            anvil_asm_append(out, "\tcvtsi2ss ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_CVTSI2SD:
            anvil_asm_append(out, "\tcvtsi2sd ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_CVTSS2SI:
            anvil_asm_append(out, "\tcvtss2si ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_CVTSD2SI:
            anvil_asm_append(out, "\tcvtsd2si ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_CVTSS2SD:
            anvil_asm_append(out, "\tcvtss2sd ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_CVTSD2SS:
            anvil_asm_append(out, "\tcvtsd2ss ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        default:
            anvil_asm_append(out, "\t# unknown instruction\n");
            break;
    }
}

void x86_64_emit_prologue(AnvilBackend* backend, AnvilMFunc* func, AnvilAsmBuffer* out) {
    (void)backend;
    
    anvil_asm_append(out, "\tpushq %%rbp\n");
    anvil_asm_append(out, "\tmovq %%rsp, %%rbp\n");
    
    if (func->stack_size > 0) {
        int aligned_size = (func->stack_size + 15) & ~15;
        anvil_asm_append(out, "\tsubq $%d, %%rsp\n", aligned_size);
    }
}

void x86_64_emit_epilogue(AnvilBackend* backend, AnvilMFunc* func, AnvilAsmBuffer* out) {
    (void)backend;
    (void)func;
    
    anvil_asm_append(out, "\tmovq %%rbp, %%rsp\n");
    anvil_asm_append(out, "\tpopq %%rbp\n");
}

void x86_64_emit_label(AnvilBackend* backend, const char* label, AnvilAsmBuffer* out) {
    (void)backend;
    anvil_asm_append(out, "%s:\n", label);
}

void x86_64_emit_data(AnvilBackend* backend, void* data, AnvilAsmBuffer* out) {
    (void)backend;
    (void)data;
    (void)out;
}

void x86_64_emit_func(AnvilBackend* backend, AnvilMFunc* func, AnvilAsmBuffer* out) {
    anvil_asm_append(out, "\t.globl %s\n", func->name);
    anvil_asm_append(out, "\t.type %s, @function\n", func->name);
    anvil_asm_append(out, "%s:\n", func->name);
    
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
    
    anvil_asm_append(out, "\t.size %s, .-%s\n", func->name, func->name);
}

void x86_64_emit_mir(AnvilBackend* backend, AnvilMIR* mir, AnvilAsmBuffer* out) {
    anvil_asm_append(out, "\t.text\n");
    
    for (AnvilMFunc* func = mir->first_func; func; func = func->next) {
        x86_64_emit_func(backend, func, out);
        anvil_asm_append(out, "\n");
    }
}
