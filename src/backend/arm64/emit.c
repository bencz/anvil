#include "emit.h"
#include "../../core/str.h"
#include <stdio.h>
#include <string.h>

static const char* arm64_fp_reg_name(int id, int size) {
    static char buf[8];
    int reg_num = id;
    if (id >= ARM64_V0 && id <= ARM64_V0 + 31) {
        reg_num = id - ARM64_V0;
    }
    if (size == 4) {
        snprintf(buf, sizeof(buf), "s%d", reg_num);
    } else {
        snprintf(buf, sizeof(buf), "d%d", reg_num);
    }
    return buf;
}

static void emit_operand(AnvilBackend* backend, AnvilMOperand* op, AnvilAsmBuffer* out) {
    switch (op->kind) {
        case ANVIL_MOP_PREG:
            if (op->is_fp) {
                anvil_asm_append(out, "%s", arm64_fp_reg_name(op->preg.id, op->size));
            } else {
                anvil_asm_append(out, "%s", arm64_reg_name(op->preg.id, op->size * 8));
            }
            break;
        case ANVIL_MOP_VREG:
            if (op->is_fp) {
                anvil_asm_append(out, "d%d", op->vreg.id);
            } else {
                anvil_asm_append(out, "v%d", op->vreg.id);
            }
            break;
        case ANVIL_MOP_IMM:
            anvil_asm_append(out, "#%lld", (long long)op->imm.value);
            break;
        case ANVIL_MOP_MEM:
            anvil_asm_append(out, "[");
            if (op->mem.base_reg >= 0) {
                if (op->mem.base_is_vreg) {
                    anvil_asm_append(out, "v%d", op->mem.base_reg);
                } else {
                    anvil_asm_append(out, "%s", arm64_reg_name(op->mem.base_reg, 64));
                }
            }
            if (op->mem.disp != 0) {
                anvil_asm_append(out, ", #%lld", (long long)op->mem.disp);
            }
            if (op->mem.index_reg >= 0) {
                anvil_asm_append(out, ", ");
                if (op->mem.index_is_vreg) {
                    anvil_asm_append(out, "v%d", op->mem.index_reg);
                } else {
                    anvil_asm_append(out, "%s", arm64_reg_name(op->mem.index_reg, 64));
                }
                if (op->mem.scale > 1) {
                    anvil_asm_append(out, ", lsl #%d", op->mem.scale == 2 ? 1 : op->mem.scale == 4 ? 2 : 3);
                }
            }
            anvil_asm_append(out, "]");
            break;
        case ANVIL_MOP_LABEL:
            anvil_asm_append(out, "%s", op->label.name);
            break;
        case ANVIL_MOP_FUNC:
            anvil_asm_append(out, "%s", op->func.name);
            break;
        case ANVIL_MOP_GLOBAL:
            anvil_asm_append(out, "%s", op->global.name);
            break;
        default:
            break;
    }
    (void)backend;
}

static const char* cc_suffix(AnvilCondCode cc) {
    switch (cc) {
        case ANVIL_CC_EQ: return "eq";
        case ANVIL_CC_NE: return "ne";
        case ANVIL_CC_LT: return "lt";
        case ANVIL_CC_LE: return "le";
        case ANVIL_CC_GT: return "gt";
        case ANVIL_CC_GE: return "ge";
        case ANVIL_CC_ULT: return "lo";
        case ANVIL_CC_ULE: return "ls";
        case ANVIL_CC_UGT: return "hi";
        case ANVIL_CC_UGE: return "hs";
        case ANVIL_CC_S: return "mi";
        case ANVIL_CC_NS: return "pl";
        case ANVIL_CC_O: return "vs";
        case ANVIL_CC_NO: return "vc";
        default: return "eq";
    }
}

void arm64_emit_instruction(AnvilBackend* backend, AnvilMInst* inst, AnvilAsmBuffer* out) {
    switch (inst->kind) {
        case ANVIL_MIR_NOP:
            anvil_asm_append(out, "\tnop\n");
            break;
            
        case ANVIL_MIR_MOV:
        case ANVIL_MIR_MOV_IMM:
            anvil_asm_append(out, "\tmov ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_MOVSS:
        case ANVIL_MIR_MOVSD:
            anvil_asm_append(out, "\tfmov ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_LOAD:
            anvil_asm_append(out, "\tldr ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_STORE:
            anvil_asm_append(out, "\tstr ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_ADD:
            anvil_asm_append(out, "\tadd ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_SUB:
            anvil_asm_append(out, "\tsub ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_MUL:
        case ANVIL_MIR_IMUL:
            anvil_asm_append(out, "\tmul ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_DIV:
            anvil_asm_append(out, "\tudiv ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_IDIV:
            anvil_asm_append(out, "\tsdiv ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_NEG:
            anvil_asm_append(out, "\tneg ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_AND:
            anvil_asm_append(out, "\tand ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_OR:
            anvil_asm_append(out, "\torr ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_XOR:
            anvil_asm_append(out, "\teor ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_NOT:
            anvil_asm_append(out, "\tmvn ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_SHL:
            anvil_asm_append(out, "\tlsl ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_SHR:
            anvil_asm_append(out, "\tlsr ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_SAR:
            anvil_asm_append(out, "\tasr ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_CMP:
            anvil_asm_append(out, "\tcmp ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_TEST:
            anvil_asm_append(out, "\ttst ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_SETCC:
            anvil_asm_append(out, "\tcset ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", %s\n", cc_suffix(inst->cc));
            break;
            
        case ANVIL_MIR_JMP:
            anvil_asm_append(out, "\tb ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_JCC:
            anvil_asm_append(out, "\tb.%s ", cc_suffix(inst->cc));
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_RET:
            anvil_asm_append(out, "\tret\n");
            break;
            
        case ANVIL_MIR_CALL: {
            const AnvilABI* abi = inst->func ? inst->func->abi : NULL;
            const char* prefix = (abi && abi->uses_underscore_prefix) ? "_" : "";
            bool variadic_on_stack = abi && abi->variadic_args_on_stack;
            
            bool is_variadic = inst->is_variadic && variadic_on_stack;
            int num_fixed_args = is_variadic ? inst->num_fixed_args : inst->num_operands - 1;
            int num_variadic_args = is_variadic ? inst->num_operands - 1 - num_fixed_args : 0;
            
            if (is_variadic && num_variadic_args > 0) {
                int stack_size = num_variadic_args * 8;
                stack_size = (stack_size + 15) & ~15;
                anvil_asm_append(out, "\tsub sp, sp, #%d\n", stack_size);
                
                for (int i = num_fixed_args + 1; i < inst->num_operands; i++) {
                    AnvilMOperand* arg = &inst->operands[i];
                    int offset = (i - num_fixed_args - 1) * 8;
                    
                    if (arg->kind == ANVIL_MOP_IMM) {
                        anvil_asm_append(out, "\tmov x8, #%lld\n", (long long)arg->imm.value);
                        anvil_asm_append(out, "\tstr x8, [sp, #%d]\n", offset);
                    } else if (arg->kind == ANVIL_MOP_PREG) {
                        const char* src = arm64_reg_name(arg->preg.id, 64);
                        anvil_asm_append(out, "\tstr %s, [sp, #%d]\n", src, offset);
                    } else if (arg->kind == ANVIL_MOP_VREG) {
                        anvil_asm_append(out, "\tstr v%d, [sp, #%d]\n", arg->vreg.id, offset);
                    }
                }
            }
            
            for (int i = num_fixed_args; i >= 1; i--) {
                AnvilMOperand* arg = &inst->operands[i];
                int arg_preg = abi ? abi->arg_regs_int[i-1] : (i-1);
                const char* reg = arm64_reg_name(arg_preg, arg->size <= 4 ? 32 : 64);
                
                if (arg->kind == ANVIL_MOP_IMM) {
                    anvil_asm_append(out, "\tmov %s, #%lld\n", reg, (long long)arg->imm.value);
                } else if (arg->kind == ANVIL_MOP_PREG) {
                    const char* src = arm64_reg_name(arg->preg.id, arg->size * 8);
                    if (strcmp(src, reg) != 0) {
                        anvil_asm_append(out, "\tmov %s, %s\n", reg, src);
                    }
                } else if (arg->kind == ANVIL_MOP_VREG) {
                    anvil_asm_append(out, "\tmov %s, v%d\n", reg, arg->vreg.id);
                } else if (arg->kind == ANVIL_MOP_LABEL) {
                    const char* reg64 = arm64_reg_name(arg_preg, 64);
                    anvil_asm_append(out, "\tadrp %s, %s@PAGE\n", reg64, arg->label.name);
                    anvil_asm_append(out, "\tadd %s, %s, %s@PAGEOFF\n", reg64, reg64, arg->label.name);
                }
            }
            
            if (inst->operands[0].kind == ANVIL_MOP_FUNC) {
                anvil_asm_append(out, "\tbl %s%s\n", prefix, inst->operands[0].func.name);
            } else {
                anvil_asm_append(out, "\tbl ");
                emit_operand(backend, &inst->operands[0], out);
                anvil_asm_append(out, "\n");
            }
            
            if (is_variadic && num_variadic_args > 0) {
                int stack_size = num_variadic_args * 8;
                stack_size = (stack_size + 15) & ~15;
                anvil_asm_append(out, "\tadd sp, sp, #%d\n", stack_size);
            }
            break;
        }
            
        case ANVIL_MIR_LABEL:
            anvil_asm_append(out, "%s:\n", inst->operands[0].label.name);
            break;
            
        case ANVIL_MIR_COMMENT:
            if (inst->comment) {
                anvil_asm_append(out, "\t// %s\n", inst->comment);
            }
            break;
            
        case ANVIL_MIR_FADD: {
            anvil_asm_append(out, "\tfadd ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
        }
            
        case ANVIL_MIR_FSUB: {
            anvil_asm_append(out, "\tfsub ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
        }
            
        case ANVIL_MIR_FMUL: {
            anvil_asm_append(out, "\tfmul ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
        }
            
        case ANVIL_MIR_FDIV: {
            anvil_asm_append(out, "\tfdiv ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
        }
            
        case ANVIL_MIR_FNEG: {
            anvil_asm_append(out, "\tfneg ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, "\n");
            break;
        }
            
        case ANVIL_MIR_FCMP: {
            const char* prefix = inst->operands[0].size == 4 ? "s" : "d";
            anvil_asm_append(out, "\tfcmp %s", prefix);
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
        }
            
        case ANVIL_MIR_CVTSI2SS:
            anvil_asm_append(out, "\tscvtf s");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_CVTSI2SD:
            anvil_asm_append(out, "\tscvtf d");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_CVTSS2SI:
        case ANVIL_MIR_CVTSD2SI:
            anvil_asm_append(out, "\tfcvtzs ");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", ");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_CVTSS2SD:
            anvil_asm_append(out, "\tfcvt d");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", s");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        case ANVIL_MIR_CVTSD2SS:
            anvil_asm_append(out, "\tfcvt s");
            emit_operand(backend, &inst->operands[0], out);
            anvil_asm_append(out, ", d");
            emit_operand(backend, &inst->operands[1], out);
            anvil_asm_append(out, "\n");
            break;
            
        default:
            anvil_asm_append(out, "\t// unknown instruction\n");
            break;
    }
}

void arm64_emit_prologue(AnvilBackend* backend, AnvilMFunc* func, AnvilAsmBuffer* out) {
    (void)backend;
    
    anvil_asm_append(out, "\tstp x29, x30, [sp, #-16]!\n");
    anvil_asm_append(out, "\tmov x29, sp\n");
    
    if (func->stack_size > 0) {
        int aligned_size = (func->stack_size + 15) & ~15;
        anvil_asm_append(out, "\tsub sp, sp, #%d\n", aligned_size);
    }
}

void arm64_emit_epilogue(AnvilBackend* backend, AnvilMFunc* func, AnvilAsmBuffer* out) {
    (void)backend;
    
    if (func->stack_size > 0) {
        int aligned_size = (func->stack_size + 15) & ~15;
        anvil_asm_append(out, "\tadd sp, sp, #%d\n", aligned_size);
    }
    
    anvil_asm_append(out, "\tldp x29, x30, [sp], #16\n");
}

void arm64_emit_label(AnvilBackend* backend, const char* label, AnvilAsmBuffer* out) {
    (void)backend;
    anvil_asm_append(out, "%s:\n", label);
}

void arm64_emit_data(AnvilBackend* backend, void* data, AnvilAsmBuffer* out) {
    (void)backend;
    (void)data;
    (void)out;
}

void arm64_emit_func(AnvilBackend* backend, AnvilMFunc* func, AnvilAsmBuffer* out) {
    anvil_asm_append(out, "\t.globl %s\n", func->name);
    anvil_asm_append(out, "\t.type %s, %%function\n", func->name);
    anvil_asm_append(out, "%s:\n", func->name);
    
    arm64_emit_prologue(backend, func, out);
    
    for (size_t i = 0; i < anvil_vec_len(&func->blocks); i++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, i);
        
        for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
            if (inst->kind == ANVIL_MIR_RET) {
                arm64_emit_epilogue(backend, func, out);
            }
            arm64_emit_instruction(backend, inst, out);
        }
    }
    
    anvil_asm_append(out, "\t.size %s, .-%s\n", func->name, func->name);
}

void arm64_emit_mir(AnvilBackend* backend, AnvilMIR* mir, AnvilAsmBuffer* out) {
    anvil_asm_append(out, "\t.text\n");
    
    for (AnvilMFunc* func = mir->first_func; func; func = func->next) {
        arm64_emit_func(backend, func, out);
        anvil_asm_append(out, "\n");
    }
}
