#include "emit.h"
#include "target.h"
#include "../../core/str.h"
#include "../../core/endian.h"
#include <stdio.h>
#include <string.h>

static const char* ppc64_cc_suffix(AnvilCondCode cc) {
    switch (cc) {
        case ANVIL_CC_EQ: return "eq";
        case ANVIL_CC_NE: return "ne";
        case ANVIL_CC_LT: return "lt";
        case ANVIL_CC_LE: return "le";
        case ANVIL_CC_GT: return "gt";
        case ANVIL_CC_GE: return "ge";
        case ANVIL_CC_ULT: return "lt";
        case ANVIL_CC_ULE: return "le";
        case ANVIL_CC_UGT: return "gt";
        case ANVIL_CC_UGE: return "ge";
        default: return "";
    }
}

static void ppc64_emit_load_imm(AnvilAsmBuffer* out, const char* reg, int64_t imm) {
    if (imm >= -32768 && imm <= 32767) {
        anvil_asm_append(out, "\tli %s, %lld\n", reg, (long long)imm);
    } else if ((imm & 0xFFFF) == 0 && imm >= -2147483648LL && imm <= 2147483647LL) {
        anvil_asm_append(out, "\tlis %s, %lld\n", reg, (long long)(imm >> 16));
    } else if (imm >= -2147483648LL && imm <= 2147483647LL) {
        anvil_asm_append(out, "\tlis %s, %lld\n", reg, (long long)((imm >> 16) & 0xFFFF));
        anvil_asm_append(out, "\tori %s, %s, %lld\n", reg, reg, (long long)(imm & 0xFFFF));
    } else {
        int64_t hi = (imm >> 32) & 0xFFFFFFFF;
        int64_t lo = imm & 0xFFFFFFFF;
        
        anvil_asm_append(out, "\tlis %s, %lld\n", reg, (long long)((hi >> 16) & 0xFFFF));
        anvil_asm_append(out, "\tori %s, %s, %lld\n", reg, reg, (long long)(hi & 0xFFFF));
        anvil_asm_append(out, "\tsldi %s, %s, 32\n", reg, reg);
        anvil_asm_append(out, "\toris %s, %s, %lld\n", reg, reg, (long long)((lo >> 16) & 0xFFFF));
        anvil_asm_append(out, "\tori %s, %s, %lld\n", reg, reg, (long long)(lo & 0xFFFF));
    }
}

static void ppc64_emit_load_addr(AnvilAsmBuffer* out, const char* reg, const char* label) {
    anvil_asm_append(out, "\taddis %s, 2, %s@toc@ha\n", reg, label);
    anvil_asm_append(out, "\taddi %s, %s, %s@toc@l\n", reg, reg, label);
}

static const char* ppc64_get_reg(AnvilMOperand* op) {
    if (op->kind == ANVIL_MOP_PREG) {
        return ppc64_reg_name(op->preg.id);
    }
    return "r0";
}

static const char* ppc64_load_suffix(int size) {
    switch (size) {
        case 1: return "bz";
        case 2: return "hz";
        case 4: return "wz";
        case 8: return "d";
        default: return "d";
    }
}

static const char* ppc64_store_suffix(int size) {
    switch (size) {
        case 1: return "b";
        case 2: return "h";
        case 4: return "w";
        case 8: return "d";
        default: return "d";
    }
}

static const char* ppc64_arith_suffix(int size) {
    return (size <= 4) ? "w" : "d";
}

void ppc64_emit_instruction(AnvilBackend* backend, AnvilMInst* inst, AnvilAsmBuffer* out) {
    const AnvilABI* abi = inst->func ? inst->func->abi : NULL;
    int size = inst->num_operands > 0 ? inst->operands[0].size : 8;
    if (size == 0) size = 8;
    const char* suffix = ppc64_arith_suffix(size);
    
    switch (inst->kind) {
        case ANVIL_MIR_NOP:
            anvil_asm_append(out, "\tnop\n");
            break;
            
        case ANVIL_MIR_MOV: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            if (inst->operands[1].kind == ANVIL_MOP_IMM) {
                ppc64_emit_load_imm(out, dst, inst->operands[1].imm.value);
            } else if (inst->operands[1].kind == ANVIL_MOP_PREG) {
                const char* src = ppc64_get_reg(&inst->operands[1]);
                if (strcmp(dst, src) != 0) {
                    anvil_asm_append(out, "\tmr %s, %s\n", dst, src);
                }
            } else if (inst->operands[1].kind == ANVIL_MOP_LABEL) {
                ppc64_emit_load_addr(out, dst, inst->operands[1].label.name);
            }
            break;
        }
            
        case ANVIL_MIR_ADD: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            if (inst->operands[1].kind == ANVIL_MOP_IMM) {
                int64_t imm = inst->operands[1].imm.value;
                if (imm >= -32768 && imm <= 32767) {
                    anvil_asm_append(out, "\taddi %s, %s, %lld\n", dst, dst, (long long)imm);
                } else {
                    anvil_asm_append(out, "\taddis %s, %s, %lld\n", dst, dst, (long long)((imm >> 16) & 0xFFFF));
                    if (imm & 0xFFFF) {
                        anvil_asm_append(out, "\taddi %s, %s, %lld\n", dst, dst, (long long)(imm & 0xFFFF));
                    }
                }
            } else {
                const char* src = ppc64_get_reg(&inst->operands[1]);
                anvil_asm_append(out, "\tadd %s, %s, %s\n", dst, dst, src);
            }
            break;
        }
            
        case ANVIL_MIR_SUB: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            if (inst->operands[1].kind == ANVIL_MOP_IMM) {
                int64_t imm = -inst->operands[1].imm.value;
                if (imm >= -32768 && imm <= 32767) {
                    anvil_asm_append(out, "\taddi %s, %s, %lld\n", dst, dst, (long long)imm);
                } else {
                    anvil_asm_append(out, "\tlis r0, %lld\n", (long long)((inst->operands[1].imm.value >> 16) & 0xFFFF));
                    anvil_asm_append(out, "\tori r0, r0, %lld\n", (long long)(inst->operands[1].imm.value & 0xFFFF));
                    anvil_asm_append(out, "\tsub%s %s, %s, r0\n", suffix, dst, dst);
                }
            } else {
                const char* src = ppc64_get_reg(&inst->operands[1]);
                anvil_asm_append(out, "\tsub%s %s, %s, %s\n", suffix, dst, dst, src);
            }
            break;
        }
            
        case ANVIL_MIR_MUL: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            if (inst->operands[1].kind == ANVIL_MOP_IMM) {
                int64_t imm = inst->operands[1].imm.value;
                if (imm >= -32768 && imm <= 32767) {
                    anvil_asm_append(out, "\tmulli %s, %s, %lld\n", dst, dst, (long long)imm);
                } else {
                    ppc64_emit_load_imm(out, "r0", imm);
                    anvil_asm_append(out, "\tmull%s %s, %s, r0\n", suffix, dst, dst);
                }
            } else {
                const char* src = ppc64_get_reg(&inst->operands[1]);
                anvil_asm_append(out, "\tmull%s %s, %s, %s\n", suffix, dst, dst, src);
            }
            break;
        }
            
        case ANVIL_MIR_DIV: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            if (inst->operands[1].kind == ANVIL_MOP_IMM) {
                ppc64_emit_load_imm(out, "r0", inst->operands[1].imm.value);
                anvil_asm_append(out, "\tdiv%s %s, %s, r0\n", suffix, dst, dst);
            } else {
                const char* src = ppc64_get_reg(&inst->operands[1]);
                anvil_asm_append(out, "\tdiv%s %s, %s, %s\n", suffix, dst, dst, src);
            }
            break;
        }
            
        case ANVIL_MIR_MOD: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            const char* src;
            if (inst->operands[1].kind == ANVIL_MOP_IMM) {
                ppc64_emit_load_imm(out, "r0", inst->operands[1].imm.value);
                src = "r0";
            } else {
                src = ppc64_get_reg(&inst->operands[1]);
            }
            anvil_asm_append(out, "\tdiv%s r11, %s, %s\n", suffix, dst, src);
            anvil_asm_append(out, "\tmull%s r11, r11, %s\n", suffix, src);
            anvil_asm_append(out, "\tsub%s %s, %s, r11\n", suffix, dst, dst);
            break;
        }
            
        case ANVIL_MIR_NEG: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            anvil_asm_append(out, "\tneg %s, %s\n", dst, dst);
            break;
        }
            
        case ANVIL_MIR_AND: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            if (inst->operands[1].kind == ANVIL_MOP_IMM) {
                int64_t imm = inst->operands[1].imm.value;
                if (imm >= 0 && imm <= 65535) {
                    anvil_asm_append(out, "\tandi. %s, %s, %lld\n", dst, dst, (long long)imm);
                } else {
                    ppc64_emit_load_imm(out, "r0", imm);
                    anvil_asm_append(out, "\tand %s, %s, r0\n", dst, dst);
                }
            } else {
                const char* src = ppc64_get_reg(&inst->operands[1]);
                anvil_asm_append(out, "\tand %s, %s, %s\n", dst, dst, src);
            }
            break;
        }
            
        case ANVIL_MIR_OR: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            if (inst->operands[1].kind == ANVIL_MOP_IMM) {
                int64_t imm = inst->operands[1].imm.value;
                if (imm >= 0 && imm <= 65535) {
                    anvil_asm_append(out, "\tori %s, %s, %lld\n", dst, dst, (long long)imm);
                } else {
                    ppc64_emit_load_imm(out, "r0", imm);
                    anvil_asm_append(out, "\tor %s, %s, r0\n", dst, dst);
                }
            } else {
                const char* src = ppc64_get_reg(&inst->operands[1]);
                anvil_asm_append(out, "\tor %s, %s, %s\n", dst, dst, src);
            }
            break;
        }
            
        case ANVIL_MIR_XOR: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            if (inst->operands[1].kind == ANVIL_MOP_IMM) {
                int64_t imm = inst->operands[1].imm.value;
                if (imm >= 0 && imm <= 65535) {
                    anvil_asm_append(out, "\txori %s, %s, %lld\n", dst, dst, (long long)imm);
                } else {
                    ppc64_emit_load_imm(out, "r0", imm);
                    anvil_asm_append(out, "\txor %s, %s, r0\n", dst, dst);
                }
            } else {
                const char* src = ppc64_get_reg(&inst->operands[1]);
                anvil_asm_append(out, "\txor %s, %s, %s\n", dst, dst, src);
            }
            break;
        }
            
        case ANVIL_MIR_NOT: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            anvil_asm_append(out, "\tnot %s, %s\n", dst, dst);
            break;
        }
            
        case ANVIL_MIR_SHL: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            if (inst->operands[1].kind == ANVIL_MOP_IMM) {
                int64_t imm = inst->operands[1].imm.value;
                if (size <= 4) {
                    anvil_asm_append(out, "\tslwi %s, %s, %lld\n", dst, dst, (long long)imm);
                } else {
                    anvil_asm_append(out, "\tsldi %s, %s, %lld\n", dst, dst, (long long)imm);
                }
            } else {
                const char* src = ppc64_get_reg(&inst->operands[1]);
                if (size <= 4) {
                    anvil_asm_append(out, "\tslw %s, %s, %s\n", dst, dst, src);
                } else {
                    anvil_asm_append(out, "\tsld %s, %s, %s\n", dst, dst, src);
                }
            }
            break;
        }
            
        case ANVIL_MIR_SHR: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            if (inst->operands[1].kind == ANVIL_MOP_IMM) {
                int64_t imm = inst->operands[1].imm.value;
                if (size <= 4) {
                    anvil_asm_append(out, "\tsrwi %s, %s, %lld\n", dst, dst, (long long)imm);
                } else {
                    anvil_asm_append(out, "\tsrdi %s, %s, %lld\n", dst, dst, (long long)imm);
                }
            } else {
                const char* src = ppc64_get_reg(&inst->operands[1]);
                if (size <= 4) {
                    anvil_asm_append(out, "\tsrw %s, %s, %s\n", dst, dst, src);
                } else {
                    anvil_asm_append(out, "\tsrd %s, %s, %s\n", dst, dst, src);
                }
            }
            break;
        }
            
        case ANVIL_MIR_SAR: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            if (inst->operands[1].kind == ANVIL_MOP_IMM) {
                int64_t imm = inst->operands[1].imm.value;
                if (size <= 4) {
                    anvil_asm_append(out, "\tsrawi %s, %s, %lld\n", dst, dst, (long long)imm);
                } else {
                    anvil_asm_append(out, "\tsradi %s, %s, %lld\n", dst, dst, (long long)imm);
                }
            } else {
                const char* src = ppc64_get_reg(&inst->operands[1]);
                if (size <= 4) {
                    anvil_asm_append(out, "\tsraw %s, %s, %s\n", dst, dst, src);
                } else {
                    anvil_asm_append(out, "\tsrad %s, %s, %s\n", dst, dst, src);
                }
            }
            break;
        }
            
        case ANVIL_MIR_CMP: {
            const char* lhs = ppc64_get_reg(&inst->operands[0]);
            if (inst->operands[1].kind == ANVIL_MOP_IMM) {
                int64_t imm = inst->operands[1].imm.value;
                if (imm >= -32768 && imm <= 32767) {
                    if (size <= 4) {
                        anvil_asm_append(out, "\tcmpwi %s, %lld\n", lhs, (long long)imm);
                    } else {
                        anvil_asm_append(out, "\tcmpdi %s, %lld\n", lhs, (long long)imm);
                    }
                } else {
                    ppc64_emit_load_imm(out, "r0", imm);
                    if (size <= 4) {
                        anvil_asm_append(out, "\tcmpw %s, r0\n", lhs);
                    } else {
                        anvil_asm_append(out, "\tcmpd %s, r0\n", lhs);
                    }
                }
            } else {
                const char* rhs = ppc64_get_reg(&inst->operands[1]);
                if (size <= 4) {
                    anvil_asm_append(out, "\tcmpw %s, %s\n", lhs, rhs);
                } else {
                    anvil_asm_append(out, "\tcmpd %s, %s\n", lhs, rhs);
                }
            }
            break;
        }
            
        case ANVIL_MIR_LOAD: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            const char* lsuffix = ppc64_load_suffix(size);
            if (inst->operands[1].kind == ANVIL_MOP_MEM) {
                const char* base = ppc64_reg_name(inst->operands[1].mem.base_reg);
                int64_t disp = inst->operands[1].mem.disp;
                anvil_asm_append(out, "\tl%s %s, %lld(%s)\n", lsuffix, dst, (long long)disp, base);
            } else if (inst->operands[1].kind == ANVIL_MOP_LABEL) {
                ppc64_emit_load_addr(out, dst, inst->operands[1].label.name);
                anvil_asm_append(out, "\tl%s %s, 0(%s)\n", lsuffix, dst, dst);
            }
            break;
        }
            
        case ANVIL_MIR_STORE: {
            const char* src = ppc64_get_reg(&inst->operands[1]);
            const char* ssuffix = ppc64_store_suffix(size);
            if (inst->operands[0].kind == ANVIL_MOP_MEM) {
                const char* base = ppc64_reg_name(inst->operands[0].mem.base_reg);
                int64_t disp = inst->operands[0].mem.disp;
                anvil_asm_append(out, "\tst%s %s, %lld(%s)\n", ssuffix, src, (long long)disp, base);
            }
            break;
        }
            
        case ANVIL_MIR_LEA: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            if (inst->operands[1].kind == ANVIL_MOP_MEM) {
                const char* base = ppc64_reg_name(inst->operands[1].mem.base_reg);
                int64_t disp = inst->operands[1].mem.disp;
                if (disp == 0) {
                    if (strcmp(dst, base) != 0) {
                        anvil_asm_append(out, "\tmr %s, %s\n", dst, base);
                    }
                } else {
                    anvil_asm_append(out, "\taddi %s, %s, %lld\n", dst, base, (long long)disp);
                }
            } else if (inst->operands[1].kind == ANVIL_MOP_LABEL) {
                ppc64_emit_load_addr(out, dst, inst->operands[1].label.name);
            }
            break;
        }
            
        case ANVIL_MIR_JMP:
            if (inst->operands[0].kind == ANVIL_MOP_LABEL) {
                anvil_asm_append(out, "\tb %s\n", inst->operands[0].label.name);
            }
            break;
            
        case ANVIL_MIR_JCC:
            if (inst->operands[0].kind == ANVIL_MOP_LABEL) {
                anvil_asm_append(out, "\tb%s %s\n", ppc64_cc_suffix(inst->cc), 
                                inst->operands[0].label.name);
            }
            break;
            
        case ANVIL_MIR_RET:
            anvil_asm_append(out, "\tblr\n");
            break;
            
        case ANVIL_MIR_CALL: {
            int num_args = inst->num_operands - 1;
            int num_fixed = inst->is_variadic ? inst->num_fixed_args : num_args;
            
            for (int i = num_fixed; i >= 1; i--) {
                AnvilMOperand* arg = &inst->operands[i];
                int arg_idx = i - 1;
                if (arg_idx >= 8) continue;
                
                int arg_preg = abi ? abi->arg_regs_int[arg_idx] : (PPC64_R3 + arg_idx);
                const char* reg = ppc64_reg_name(arg_preg);
                
                if (arg->kind == ANVIL_MOP_IMM) {
                    ppc64_emit_load_imm(out, reg, arg->imm.value);
                } else if (arg->kind == ANVIL_MOP_PREG) {
                    const char* src = ppc64_reg_name(arg->preg.id);
                    if (strcmp(src, reg) != 0) {
                        anvil_asm_append(out, "\tmr %s, %s\n", reg, src);
                    }
                } else if (arg->kind == ANVIL_MOP_LABEL) {
                    ppc64_emit_load_addr(out, reg, arg->label.name);
                }
            }
            
            if (inst->is_variadic && num_args > num_fixed) {
                int stack_offset = 48;
                for (int i = num_fixed + 1; i <= num_args; i++) {
                    AnvilMOperand* arg = &inst->operands[i];
                    if (arg->kind == ANVIL_MOP_IMM) {
                        ppc64_emit_load_imm(out, "r0", arg->imm.value);
                        anvil_asm_append(out, "\tstd r0, %d(r1)\n", stack_offset);
                    } else if (arg->kind == ANVIL_MOP_PREG) {
                        const char* src = ppc64_reg_name(arg->preg.id);
                        anvil_asm_append(out, "\tstd %s, %d(r1)\n", src, stack_offset);
                    }
                    stack_offset += 8;
                }
            }
            
            if (inst->operands[0].kind == ANVIL_MOP_FUNC) {
                anvil_asm_append(out, "\tbl %s\n", inst->operands[0].func.name);
            }
            break;
        }
            
        case ANVIL_MIR_PUSH: {
            const char* src = ppc64_get_reg(&inst->operands[0]);
            anvil_asm_append(out, "\tstdu %s, -8(r1)\n", src);
            break;
        }
            
        case ANVIL_MIR_POP: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            anvil_asm_append(out, "\tld %s, 0(r1)\n", dst);
            anvil_asm_append(out, "\taddi r1, r1, 8\n");
            break;
        }
            
        case ANVIL_MIR_LABEL:
            if (inst->operands[0].kind == ANVIL_MOP_LABEL) {
                anvil_asm_append(out, "%s:\n", inst->operands[0].label.name);
            }
            break;
            
        case ANVIL_MIR_ZEXT: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            int src_size = inst->operands[1].size;
            if (src_size == 1) {
                anvil_asm_append(out, "\trldicl %s, %s, 0, 56\n", dst, dst);
            } else if (src_size == 2) {
                anvil_asm_append(out, "\trldicl %s, %s, 0, 48\n", dst, dst);
            } else if (src_size == 4) {
                anvil_asm_append(out, "\trldicl %s, %s, 0, 32\n", dst, dst);
            }
            break;
        }
            
        case ANVIL_MIR_SEXT: {
            const char* dst = ppc64_get_reg(&inst->operands[0]);
            int src_size = inst->operands[1].size;
            if (src_size == 1) {
                anvil_asm_append(out, "\textsb %s, %s\n", dst, dst);
            } else if (src_size == 2) {
                anvil_asm_append(out, "\textsh %s, %s\n", dst, dst);
            } else if (src_size == 4) {
                anvil_asm_append(out, "\textsw %s, %s\n", dst, dst);
            }
            break;
        }
            
        case ANVIL_MIR_TRUNC: {
            break;
        }
            
        default:
            anvil_asm_append(out, "\t# unimplemented instruction %d\n", inst->kind);
            break;
    }
    
    (void)backend;
    (void)abi;
}

void ppc64_emit_prologue(AnvilBackend* backend, AnvilMFunc* func, AnvilAsmBuffer* out) {
    bool needs_frame = func->stack_size > 0 || func->spill_slots > 0;
    bool has_calls = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
            if (inst->kind == ANVIL_MIR_CALL) {
                has_calls = true;
                break;
            }
        }
        if (has_calls) break;
    }
    
    if (!needs_frame && !has_calls) {
        return;
    }
    
    int frame_size = func->stack_size;
    if (frame_size < 32) frame_size = 32;
    frame_size = (frame_size + 15) & ~15;
    
    anvil_asm_append(out, "\tstdu r1, -%d(r1)\n", frame_size);
    
    if (has_calls) {
        anvil_asm_append(out, "\tmflr r0\n");
        anvil_asm_append(out, "\tstd r0, %d(r1)\n", frame_size + 16);
    }
    
    (void)backend;
}

void ppc64_emit_epilogue(AnvilBackend* backend, AnvilMFunc* func, AnvilAsmBuffer* out) {
    bool needs_frame = func->stack_size > 0 || func->spill_slots > 0;
    bool has_calls = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
            if (inst->kind == ANVIL_MIR_CALL) {
                has_calls = true;
                break;
            }
        }
        if (has_calls) break;
    }
    
    if (!needs_frame && !has_calls) {
        return;
    }
    
    int frame_size = func->stack_size;
    if (frame_size < 32) frame_size = 32;
    frame_size = (frame_size + 15) & ~15;
    
    if (has_calls) {
        anvil_asm_append(out, "\tld r0, %d(r1)\n", frame_size + 16);
        anvil_asm_append(out, "\tmtlr r0\n");
    }
    
    anvil_asm_append(out, "\taddi r1, r1, %d\n", frame_size);
    
    (void)backend;
}
