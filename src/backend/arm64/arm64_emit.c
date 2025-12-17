/*
 * ANVIL - ARM64 Backend - Instruction Emission
 * 
 * Functions for emitting ARM64 instructions from IR.
 */

#include "arm64_internal.h"
#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Value Loading
 * ============================================================================ */

void arm64_emit_load_value(arm64_backend_t *be, anvil_value_t *val, int target_reg)
{
    if (!val) return;
    
    const char *xreg = arm64_xreg_names[target_reg];
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_INT:
            arm64_emit_mov_imm(be, target_reg, val->data.i);
            break;
            
        case ANVIL_VAL_CONST_FLOAT:
            anvil_strbuf_appendf(&be->code, "\tldr %s, =%lld\n", xreg, 
                (long long)*(uint64_t*)&val->data.f);
            break;
            
        case ANVIL_VAL_CONST_NULL:
            anvil_strbuf_appendf(&be->code, "\tmov %s, #0\n", xreg);
            break;
            
        case ANVIL_VAL_CONST_STRING:
            {
                const char *label = arm64_add_string(be, val->data.str);
                if (arm64_is_darwin(be)) {
                    anvil_strbuf_appendf(&be->code, "\tadrp %s, %s@PAGE\n", xreg, label);
                    anvil_strbuf_appendf(&be->code, "\tadd %s, %s, %s@PAGEOFF\n", xreg, xreg, label);
                } else {
                    anvil_strbuf_appendf(&be->code, "\tadrp %s, %s\n", xreg, label);
                    anvil_strbuf_appendf(&be->code, "\tadd %s, %s, :lo12:%s\n", xreg, xreg, label);
                }
            }
            break;
            
        case ANVIL_VAL_PARAM:
            {
                int offset = arm64_get_stack_slot(be, val);
                if (offset >= 0) {
                    int size = val->type ? arm64_type_size(val->type) : 8;
                    bool is_signed = val->type ? arm64_type_is_signed(val->type) : false;
                    arm64_emit_load_from_stack_signed(be, target_reg, offset, size, is_signed);
                } else {
                    size_t idx = val->data.param.index;
                    if (idx < ARM64_NUM_ARG_REGS && target_reg != (int)idx) {
                        anvil_strbuf_appendf(&be->code, "\tmov %s, %s\n",
                            xreg, arm64_xreg_names[idx]);
                    }
                }
            }
            break;
            
        case ANVIL_VAL_INSTR:
            if (val->data.instr && val->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = arm64_get_stack_slot(be, val);
                if (offset >= 0) {
                    if (offset <= 4095) {
                        anvil_strbuf_appendf(&be->code, "\tsub %s, x29, #%d\n", xreg, offset);
                    } else {
                        anvil_strbuf_appendf(&be->code, "\tmov x16, #%d\n", offset);
                        anvil_strbuf_appendf(&be->code, "\tsub %s, x29, x16\n", xreg);
                    }
                }
            } else {
                int offset = arm64_get_stack_slot(be, val);
                if (offset >= 0) {
                    int size = val->type ? arm64_type_size(val->type) : 8;
                    bool is_signed = val->type ? arm64_type_is_signed(val->type) : false;
                    arm64_emit_load_from_stack_signed(be, target_reg, offset, size, is_signed);
                } else if (target_reg != ARM64_X0) {
                    anvil_strbuf_appendf(&be->code, "\tmov %s, x0\n", xreg);
                }
            }
            break;
            
        case ANVIL_VAL_GLOBAL:
        case ANVIL_VAL_FUNC:
            arm64_emit_load_global(be, target_reg, val->name);
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "\t// Unknown value kind %d\n", val->kind);
            break;
    }
}

/* Save instruction result to stack slot */
void arm64_save_result(arm64_backend_t *be, anvil_instr_t *instr)
{
    if (instr->result) {
        int offset = arm64_get_or_alloc_slot(be, instr->result);
        if (offset >= 0) {
            /* Use correct size based on result type */
            int size = 8;
            if (instr->result->type) {
                size = arm64_type_size(instr->result->type);
            }
            arm64_emit_store_to_stack(be, ARM64_X0, offset, size);
        }
    }
}

/* ============================================================================
 * Floating-Point Value Loading
 * ============================================================================ */

void arm64_emit_load_fp_value(arm64_backend_t *be, anvil_value_t *val, int target_dreg)
{
    if (!val) return;
    
    const char *dreg = target_dreg == 0 ? "d0" : (target_dreg == 1 ? "d1" : "d2");
    const char *sreg = target_dreg == 0 ? "s0" : (target_dreg == 1 ? "s1" : "s2");
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_FLOAT:
            if (val->type && val->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_appendf(&be->code, "\tldr %s, =0x%08x\n", sreg,
                    *(uint32_t*)&(float){(float)val->data.f});
            } else {
                anvil_strbuf_appendf(&be->code, "\tldr %s, =0x%016llx\n", dreg,
                    *(uint64_t*)&val->data.f);
            }
            break;
            
        case ANVIL_VAL_INSTR:
            if (target_dreg != 0) {
                anvil_strbuf_appendf(&be->code, "\tfmov %s, d0\n", dreg);
            }
            break;
            
        case ANVIL_VAL_PARAM:
            {
                size_t idx = val->data.param.index;
                if (idx < 8 && target_dreg != (int)idx) {
                    anvil_strbuf_appendf(&be->code, "\tfmov %s, d%zu\n", dreg, idx);
                }
            }
            break;
            
        default:
            arm64_emit_load_value(be, val, ARM64_X9);
            anvil_strbuf_appendf(&be->code, "\tfmov %s, x9\n", dreg);
            break;
    }
}

/* ============================================================================
 * PHI Node Handling
 * ============================================================================ */

void arm64_emit_phi_copies(arm64_backend_t *be, anvil_block_t *src_block, anvil_block_t *dest_block)
{
    if (!dest_block) return;
    
    for (anvil_instr_t *instr = dest_block->first; instr; instr = instr->next) {
        if (instr->op != ANVIL_OP_PHI) break;
        
        for (size_t i = 0; i < instr->num_phi_incoming; i++) {
            if (instr->phi_blocks && instr->phi_blocks[i] == src_block) {
                if (i < instr->num_operands && instr->operands[i]) {
                    arm64_emit_load_value(be, instr->operands[i], ARM64_X9);
                    if (instr->result) {
                        int offset = arm64_get_stack_slot(be, instr->result);
                        if (offset >= 0) {
                            arm64_emit_store_to_stack(be, ARM64_X9, offset, 8);
                        }
                    }
                }
                break;
            }
        }
    }
}

/* ============================================================================
 * Prologue/Epilogue
 * ============================================================================ */

void arm64_emit_prologue(arm64_backend_t *be, anvil_func_t *func)
{
    bool is_darwin = arm64_is_darwin(be);
    const char *prefix = arm64_symbol_prefix(be);
    
    if (is_darwin) {
        anvil_strbuf_appendf(&be->code, "\t.globl %s%s\n", prefix, func->name);
        anvil_strbuf_appendf(&be->code, "\t.p2align 2\n");
        anvil_strbuf_appendf(&be->code, "%s%s:\n", prefix, func->name);
    } else {
        anvil_strbuf_appendf(&be->code, "\t.globl %s\n", func->name);
        anvil_strbuf_appendf(&be->code, "\t.type %s, %%function\n", func->name);
        anvil_strbuf_appendf(&be->code, "%s:\n", func->name);
    }
    
    /* Save frame pointer and link register */
    anvil_strbuf_append(&be->code, "\tstp x29, x30, [sp, #-16]!\n");
    anvil_strbuf_append(&be->code, "\tmov x29, sp\n");
    
    /* Allocate stack space (16-byte aligned) */
    int stack_size = (be->next_stack_offset + 15) & ~15;
    if (stack_size > 0) {
        if (stack_size <= 4095) {
            anvil_strbuf_appendf(&be->code, "\tsub sp, sp, #%d\n", stack_size);
        } else {
            anvil_strbuf_appendf(&be->code, "\tmov x16, #%d\n", stack_size);
            anvil_strbuf_append(&be->code, "\tsub sp, sp, x16\n");
        }
    }
    be->frame.total_size = stack_size;
}

void arm64_emit_epilogue(arm64_backend_t *be)
{
    int stack_size = be->frame.total_size;
    
    if (stack_size > 0) {
        if (stack_size <= 4095) {
            anvil_strbuf_appendf(&be->code, "\tadd sp, sp, #%d\n", stack_size);
        } else {
            anvil_strbuf_appendf(&be->code, "\tmov x16, #%d\n", stack_size);
            anvil_strbuf_append(&be->code, "\tadd sp, sp, x16\n");
        }
    }
    
    anvil_strbuf_append(&be->code, "\tldp x29, x30, [sp], #16\n");
    anvil_strbuf_append(&be->code, "\tret\n");
}

/* ============================================================================
 * Instruction Emission
 * ============================================================================ */

void arm64_emit_instr(arm64_backend_t *be, anvil_instr_t *instr)
{
    if (!instr) return;
    
    switch (instr->op) {
        case ANVIL_OP_PHI:
            /* PHI nodes handled by arm64_emit_phi_copies */
            break;
            
        case ANVIL_OP_ALLOCA:
            /* Stack slots are pre-allocated in arm64_emit_func first pass.
             * Here we just zero-initialize the allocated space. */
            {
                int offset = arm64_get_stack_slot(be, instr->result);
                if (offset >= 0) {
                    int size = 8;
                    if (instr->result && instr->result->type && 
                        instr->result->type->kind == ANVIL_TYPE_PTR &&
                        instr->result->type->data.pointee) {
                        size = arm64_type_size(instr->result->type->data.pointee);
                    }
                    /* Zero-initialize with correct size */
                    arm64_emit_store_to_stack(be, ARM64_XZR, offset, size);
                }
            }
            break;
            
        /* Arithmetic */
        case ANVIL_OP_ADD:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tadd x0, x9, x10\n");
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_SUB:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tsub x0, x9, x10\n");
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_MUL:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tmul x0, x9, x10\n");
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_SDIV:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tsdiv x0, x9, x10\n");
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_UDIV:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tudiv x0, x9, x10\n");
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_SMOD:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tsdiv x11, x9, x10\n");
            anvil_strbuf_append(&be->code, "\tmsub x0, x11, x10, x9\n");
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_UMOD:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tudiv x11, x9, x10\n");
            anvil_strbuf_append(&be->code, "\tmsub x0, x11, x10, x9\n");
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_NEG:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            anvil_strbuf_append(&be->code, "\tneg x0, x9\n");
            arm64_save_result(be, instr);
            break;
            
        /* Bitwise */
        case ANVIL_OP_AND:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tand x0, x9, x10\n");
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_OR:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\torr x0, x9, x10\n");
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_XOR:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\teor x0, x9, x10\n");
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_NOT:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            anvil_strbuf_append(&be->code, "\tmvn x0, x9\n");
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_SHL:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tlsl x0, x9, x10\n");
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_SHR:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tlsr x0, x9, x10\n");
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_SAR:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tasr x0, x9, x10\n");
            arm64_save_result(be, instr);
            break;
            
        /* Memory */
        case ANVIL_OP_LOAD:
            arm64_emit_load(be, instr);
            break;
            
        case ANVIL_OP_STORE:
            arm64_emit_store(be, instr);
            break;
            
        case ANVIL_OP_GEP:
            arm64_emit_gep(be, instr);
            break;
            
        case ANVIL_OP_STRUCT_GEP:
            arm64_emit_struct_gep(be, instr);
            break;
            
        /* Comparisons */
        case ANVIL_OP_CMP_EQ:
        case ANVIL_OP_CMP_NE:
        case ANVIL_OP_CMP_LT:
        case ANVIL_OP_CMP_LE:
        case ANVIL_OP_CMP_GT:
        case ANVIL_OP_CMP_GE:
        case ANVIL_OP_CMP_ULT:
        case ANVIL_OP_CMP_ULE:
        case ANVIL_OP_CMP_UGT:
        case ANVIL_OP_CMP_UGE:
            arm64_emit_cmp(be, instr);
            break;
            
        /* Control flow */
        case ANVIL_OP_BR:
            arm64_emit_br(be, instr);
            break;
            
        case ANVIL_OP_BR_COND:
            arm64_emit_br_cond(be, instr);
            break;
            
        case ANVIL_OP_CALL:
            arm64_emit_call(be, instr);
            break;
            
        case ANVIL_OP_RET:
            arm64_emit_ret(be, instr);
            break;
            
        /* Type conversions */
        case ANVIL_OP_TRUNC:
        case ANVIL_OP_ZEXT:
        case ANVIL_OP_SEXT:
        case ANVIL_OP_BITCAST:
        case ANVIL_OP_PTRTOINT:
        case ANVIL_OP_INTTOPTR:
            arm64_emit_convert(be, instr);
            break;
            
        case ANVIL_OP_SELECT:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            arm64_emit_load_value(be, instr->operands[2], ARM64_X11);
            anvil_strbuf_append(&be->code, "\tcmp x9, #0\n");
            anvil_strbuf_append(&be->code, "\tcsel x0, x10, x11, ne\n");
            arm64_save_result(be, instr);
            break;
            
        /* Floating-point */
        case ANVIL_OP_FADD:
        case ANVIL_OP_FSUB:
        case ANVIL_OP_FMUL:
        case ANVIL_OP_FDIV:
        case ANVIL_OP_FNEG:
        case ANVIL_OP_FABS:
        case ANVIL_OP_FCMP:
        case ANVIL_OP_SITOFP:
        case ANVIL_OP_UITOFP:
        case ANVIL_OP_FPTOSI:
        case ANVIL_OP_FPTOUI:
        case ANVIL_OP_FPEXT:
        case ANVIL_OP_FPTRUNC:
            arm64_emit_fp(be, instr);
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "\t// Unimplemented op %d\n", instr->op);
            break;
    }
}

/* ============================================================================
 * Memory Operations
 * ============================================================================ */

void arm64_emit_load(arm64_backend_t *be, anvil_instr_t *instr)
{
    const char *ldr_instr = "ldr x0";
    int size = 8;
    bool is_signed = false;
    
    if (instr->result && instr->result->type) {
        size = arm64_type_size(instr->result->type);
        is_signed = arm64_type_is_signed(instr->result->type);
        switch (size) {
            case 1: ldr_instr = is_signed ? "ldrsb x0" : "ldrb w0"; break;
            case 2: ldr_instr = is_signed ? "ldrsh x0" : "ldrh w0"; break;
            case 4: ldr_instr = is_signed ? "ldrsw x0" : "ldr w0"; break;
            default: ldr_instr = "ldr x0"; break;
        }
    }
    
    /* Load from alloca */
    if (instr->operands[0]->kind == ANVIL_VAL_INSTR &&
        instr->operands[0]->data.instr &&
        instr->operands[0]->data.instr->op == ANVIL_OP_ALLOCA) {
        int offset = arm64_get_stack_slot(be, instr->operands[0]);
        if (offset >= 0) {
            arm64_emit_load_from_stack_signed(be, ARM64_X0, offset, size, is_signed);
            arm64_save_result(be, instr);
            return;
        }
    }
    
    /* Load from global */
    if (instr->operands[0]->kind == ANVIL_VAL_GLOBAL) {
        const char *prefix = arm64_symbol_prefix(be);
        if (arm64_is_darwin(be)) {
            anvil_strbuf_appendf(&be->code, "\tadrp x9, %s%s@PAGE\n", 
                prefix, instr->operands[0]->name);
            anvil_strbuf_appendf(&be->code, "\t%s, [x9, %s%s@PAGEOFF]\n", 
                ldr_instr, prefix, instr->operands[0]->name);
        } else {
            anvil_strbuf_appendf(&be->code, "\tadrp x9, %s\n", instr->operands[0]->name);
            anvil_strbuf_appendf(&be->code, "\t%s, [x9, :lo12:%s]\n", 
                ldr_instr, instr->operands[0]->name);
        }
        arm64_save_result(be, instr);
        return;
    }
    
    /* Generic load */
    arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
    anvil_strbuf_appendf(&be->code, "\t%s, [x9]\n", ldr_instr);
    arm64_save_result(be, instr);
}

void arm64_emit_store(arm64_backend_t *be, anvil_instr_t *instr)
{
    const char *str_instr = "str x9";
    int size = 8;
    
    /* Determine size from the source value type (what we're storing) */
    if (instr->operands[0] && instr->operands[0]->type) {
        size = arm64_type_size(instr->operands[0]->type);
    }
    
    /* For alloca destinations, use the pointee type size */
    if (instr->operands[1] && instr->operands[1]->kind == ANVIL_VAL_INSTR &&
        instr->operands[1]->data.instr &&
        instr->operands[1]->data.instr->op == ANVIL_OP_ALLOCA) {
        if (instr->operands[1]->type &&
            instr->operands[1]->type->kind == ANVIL_TYPE_PTR &&
            instr->operands[1]->type->data.pointee) {
            size = arm64_type_size(instr->operands[1]->type->data.pointee);
        }
    }
    
    switch (size) {
        case 1: str_instr = "strb w9"; break;
        case 2: str_instr = "strh w9"; break;
        case 4: str_instr = "str w9"; break;
        default: str_instr = "str x9"; break;
    }
    
    /* Store to alloca */
    if (instr->operands[1]->kind == ANVIL_VAL_INSTR &&
        instr->operands[1]->data.instr &&
        instr->operands[1]->data.instr->op == ANVIL_OP_ALLOCA) {
        int offset = arm64_get_stack_slot(be, instr->operands[1]);
        if (offset >= 0) {
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_store_to_stack(be, ARM64_X9, offset, size);
            return;
        }
    }
    
    /* Store to global */
    if (instr->operands[1]->kind == ANVIL_VAL_GLOBAL) {
        const char *prefix = arm64_symbol_prefix(be);
        arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
        if (arm64_is_darwin(be)) {
            anvil_strbuf_appendf(&be->code, "\tadrp x10, %s%s@PAGE\n", 
                prefix, instr->operands[1]->name);
            anvil_strbuf_appendf(&be->code, "\t%s, [x10, %s%s@PAGEOFF]\n", 
                str_instr, prefix, instr->operands[1]->name);
        } else {
            anvil_strbuf_appendf(&be->code, "\tadrp x10, %s\n", instr->operands[1]->name);
            anvil_strbuf_appendf(&be->code, "\t%s, [x10, :lo12:%s]\n", 
                str_instr, instr->operands[1]->name);
        }
        return;
    }
    
    /* Generic store */
    arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
    arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
    anvil_strbuf_appendf(&be->code, "\t%s, [x10]\n", str_instr);
}

void arm64_emit_gep(arm64_backend_t *be, anvil_instr_t *instr)
{
    arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
    
    if (instr->num_operands > 1) {
        arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
        
        int elem_size = 8;
        if (instr->result && instr->result->type &&
            instr->result->type->kind == ANVIL_TYPE_PTR &&
            instr->result->type->data.pointee) {
            elem_size = arm64_type_size(instr->result->type->data.pointee);
        }
        
        switch (elem_size) {
            case 1: anvil_strbuf_append(&be->code, "\tadd x0, x9, x10\n"); break;
            case 2: anvil_strbuf_append(&be->code, "\tadd x0, x9, x10, lsl #1\n"); break;
            case 4: anvil_strbuf_append(&be->code, "\tadd x0, x9, x10, lsl #2\n"); break;
            default: anvil_strbuf_append(&be->code, "\tadd x0, x9, x10, lsl #3\n"); break;
        }
    } else {
        anvil_strbuf_append(&be->code, "\tmov x0, x9\n");
    }
    arm64_save_result(be, instr);
}

void arm64_emit_struct_gep(arm64_backend_t *be, anvil_instr_t *instr)
{
    arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
    
    int offset = 0;
    if (instr->aux_type && instr->aux_type->kind == ANVIL_TYPE_STRUCT &&
        instr->num_operands > 1 && instr->operands[1]->kind == ANVIL_VAL_CONST_INT) {
        unsigned field_idx = (unsigned)instr->operands[1]->data.i;
        if (field_idx < instr->aux_type->data.struc.num_fields) {
            offset = (int)instr->aux_type->data.struc.offsets[field_idx];
        }
    }
    
    if (offset == 0) {
        anvil_strbuf_append(&be->code, "\tmov x0, x9\n");
    } else if (offset <= 4095) {
        anvil_strbuf_appendf(&be->code, "\tadd x0, x9, #%d\n", offset);
    } else {
        anvil_strbuf_appendf(&be->code, "\tmov x10, #%d\n", offset);
        anvil_strbuf_append(&be->code, "\tadd x0, x9, x10\n");
    }
    arm64_save_result(be, instr);
}

/* ============================================================================
 * Comparison Operations
 * ============================================================================ */

void arm64_emit_cmp(arm64_backend_t *be, anvil_instr_t *instr)
{
    arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
    arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
    anvil_strbuf_append(&be->code, "\tcmp x9, x10\n");
    
    const char *cond;
    switch (instr->op) {
        case ANVIL_OP_CMP_EQ:  cond = "eq"; break;
        case ANVIL_OP_CMP_NE:  cond = "ne"; break;
        case ANVIL_OP_CMP_LT:  cond = "lt"; break;
        case ANVIL_OP_CMP_LE:  cond = "le"; break;
        case ANVIL_OP_CMP_GT:  cond = "gt"; break;
        case ANVIL_OP_CMP_GE:  cond = "ge"; break;
        case ANVIL_OP_CMP_ULT: cond = "lo"; break;
        case ANVIL_OP_CMP_ULE: cond = "ls"; break;
        case ANVIL_OP_CMP_UGT: cond = "hi"; break;
        case ANVIL_OP_CMP_UGE: cond = "hs"; break;
        default: cond = "eq"; break;
    }
    anvil_strbuf_appendf(&be->code, "\tcset x0, %s\n", cond);
    arm64_save_result(be, instr);
}

/* ============================================================================
 * Control Flow
 * ============================================================================ */

void arm64_emit_br(arm64_backend_t *be, anvil_instr_t *instr)
{
    if (instr->true_block) {
        arm64_emit_phi_copies(be, instr->parent, instr->true_block);
        anvil_strbuf_appendf(&be->code, "\tb .L%s_%s\n",
            be->current_func->name, instr->true_block->name);
    }
}

void arm64_emit_br_cond(arm64_backend_t *be, anvil_instr_t *instr)
{
    arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
    
    if (instr->true_block && instr->false_block) {
        bool true_has_phi = instr->true_block->first && 
                            instr->true_block->first->op == ANVIL_OP_PHI;
        bool false_has_phi = instr->false_block->first && 
                             instr->false_block->first->op == ANVIL_OP_PHI;
        
        if (!true_has_phi && !false_has_phi) {
            anvil_strbuf_appendf(&be->code, "\tcbnz x9, .L%s_%s\n",
                be->current_func->name, instr->true_block->name);
            anvil_strbuf_appendf(&be->code, "\tb .L%s_%s\n",
                be->current_func->name, instr->false_block->name);
        } else {
            int label_id = be->label_counter++;
            
            if (true_has_phi) {
                anvil_strbuf_appendf(&be->code, "\tcbnz x9, .Lphi_true_%d\n", label_id);
            } else {
                anvil_strbuf_appendf(&be->code, "\tcbnz x9, .L%s_%s\n",
                    be->current_func->name, instr->true_block->name);
            }
            
            if (false_has_phi) {
                arm64_emit_phi_copies(be, instr->parent, instr->false_block);
            }
            anvil_strbuf_appendf(&be->code, "\tb .L%s_%s\n",
                be->current_func->name, instr->false_block->name);
            
            if (true_has_phi) {
                anvil_strbuf_appendf(&be->code, ".Lphi_true_%d:\n", label_id);
                arm64_emit_phi_copies(be, instr->parent, instr->true_block);
                anvil_strbuf_appendf(&be->code, "\tb .L%s_%s\n",
                    be->current_func->name, instr->true_block->name);
            }
        }
    }
}

void arm64_emit_call(arm64_backend_t *be, anvil_instr_t *instr)
{
    size_t num_args = instr->num_operands - 1;
    anvil_value_t *callee = instr->operands[0];
    
    /* Check if this is a variadic function call */
    bool is_variadic = false;
    size_t num_fixed_args = 0;
    if (callee && callee->type && callee->type->kind == ANVIL_TYPE_FUNC) {
        is_variadic = callee->type->data.func.variadic;
        num_fixed_args = callee->type->data.func.num_params;
    }
    
    /* On Darwin/macOS, variadic arguments must be passed on the stack */
    if (is_variadic && arm64_is_darwin(be) && num_args > num_fixed_args) {
        size_t num_variadic = num_args - num_fixed_args;
        size_t stack_size = num_variadic * 8;
        /* Align to 16 bytes */
        stack_size = (stack_size + 15) & ~15;
        
        /* Allocate stack space for variadic args */
        if (stack_size > 0) {
            anvil_strbuf_appendf(&be->code, "\tsub sp, sp, #%zu\n", stack_size);
        }
        
        /* Load fixed arguments into registers */
        for (size_t i = 0; i < num_fixed_args && i < ARM64_NUM_ARG_REGS; i++) {
            arm64_emit_load_value(be, instr->operands[i + 1], ARM64_X9 + (int)i);
        }
        for (size_t i = 0; i < num_fixed_args && i < ARM64_NUM_ARG_REGS; i++) {
            anvil_strbuf_appendf(&be->code, "\tmov x%zu, x%d\n", i, ARM64_X9 + (int)i);
        }
        
        /* Store variadic arguments on stack */
        for (size_t i = 0; i < num_variadic; i++) {
            arm64_emit_load_value(be, instr->operands[num_fixed_args + i + 1], ARM64_X9);
            anvil_strbuf_appendf(&be->code, "\tstr x9, [sp, #%zu]\n", i * 8);
        }
        
        /* Call function */
        const char *prefix = arm64_symbol_prefix(be);
        if (callee->kind == ANVIL_VAL_FUNC ||
            (callee->kind == ANVIL_VAL_GLOBAL && callee->type && 
             callee->type->kind == ANVIL_TYPE_FUNC)) {
            anvil_strbuf_appendf(&be->code, "\tbl %s%s\n", prefix, callee->name);
        } else {
            arm64_emit_load_value(be, callee, ARM64_X9);
            anvil_strbuf_append(&be->code, "\tblr x9\n");
        }
        
        /* Restore stack */
        if (stack_size > 0) {
            anvil_strbuf_appendf(&be->code, "\tadd sp, sp, #%zu\n", stack_size);
        }
    } else {
        /* Non-variadic call or Linux - use registers */
        size_t reg_args = num_args;
        if (reg_args > ARM64_NUM_ARG_REGS) reg_args = ARM64_NUM_ARG_REGS;
        
        /* Load arguments into temporaries */
        for (size_t i = 0; i < reg_args; i++) {
            arm64_emit_load_value(be, instr->operands[i + 1], ARM64_X9 + (int)i);
        }
        
        /* Move to argument registers */
        for (size_t i = 0; i < reg_args; i++) {
            anvil_strbuf_appendf(&be->code, "\tmov x%zu, x%d\n", i, ARM64_X9 + (int)i);
        }
        
        /* Call function */
        const char *prefix = arm64_symbol_prefix(be);
        if (callee->kind == ANVIL_VAL_FUNC ||
            (callee->kind == ANVIL_VAL_GLOBAL && callee->type && 
             callee->type->kind == ANVIL_TYPE_FUNC)) {
            anvil_strbuf_appendf(&be->code, "\tbl %s%s\n", prefix, callee->name);
        } else {
            arm64_emit_load_value(be, callee, ARM64_X9);
            anvil_strbuf_append(&be->code, "\tblr x9\n");
        }
    }
    
    arm64_save_result(be, instr);
}

void arm64_emit_ret(arm64_backend_t *be, anvil_instr_t *instr)
{
    if (instr->num_operands > 0 && instr->operands[0]) {
        arm64_emit_load_value(be, instr->operands[0], ARM64_X0);
    }
    arm64_emit_epilogue(be);
}

/* ============================================================================
 * Type Conversions
 * ============================================================================ */

void arm64_emit_convert(arm64_backend_t *be, anvil_instr_t *instr)
{
    switch (instr->op) {
        case ANVIL_OP_TRUNC:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X0);
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_ZEXT:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            if (instr->operands[0]->type) {
                switch (instr->operands[0]->type->kind) {
                    case ANVIL_TYPE_I8:
                    case ANVIL_TYPE_U8:
                        anvil_strbuf_append(&be->code, "\tuxtb x0, w9\n");
                        break;
                    case ANVIL_TYPE_I16:
                    case ANVIL_TYPE_U16:
                        anvil_strbuf_append(&be->code, "\tuxth x0, w9\n");
                        break;
                    default:
                        anvil_strbuf_append(&be->code, "\tmov x0, x9\n");
                        break;
                }
            } else {
                anvil_strbuf_append(&be->code, "\tmov x0, x9\n");
            }
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_SEXT:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            if (instr->operands[0]->type) {
                switch (instr->operands[0]->type->kind) {
                    case ANVIL_TYPE_I8:
                        anvil_strbuf_append(&be->code, "\tsxtb x0, w9\n");
                        break;
                    case ANVIL_TYPE_I16:
                        anvil_strbuf_append(&be->code, "\tsxth x0, w9\n");
                        break;
                    case ANVIL_TYPE_I32:
                        anvil_strbuf_append(&be->code, "\tsxtw x0, w9\n");
                        break;
                    default:
                        anvil_strbuf_append(&be->code, "\tmov x0, x9\n");
                        break;
                }
            } else {
                anvil_strbuf_append(&be->code, "\tmov x0, x9\n");
            }
            arm64_save_result(be, instr);
            break;
            
        case ANVIL_OP_BITCAST:
        case ANVIL_OP_PTRTOINT:
        case ANVIL_OP_INTTOPTR:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X0);
            arm64_save_result(be, instr);
            break;
            
        default:
            break;
    }
}

/* ============================================================================
 * Floating-Point Operations
 * ============================================================================ */

void arm64_emit_fp(arm64_backend_t *be, anvil_instr_t *instr)
{
    bool is_f32 = (instr->result && instr->result->type &&
                   instr->result->type->kind == ANVIL_TYPE_F32) ||
                  (instr->operands[0] && instr->operands[0]->type &&
                   instr->operands[0]->type->kind == ANVIL_TYPE_F32);
    const char *reg = is_f32 ? "s" : "d";
    
    switch (instr->op) {
        case ANVIL_OP_FADD:
            arm64_emit_load_fp_value(be, instr->operands[0], 0);
            arm64_emit_load_fp_value(be, instr->operands[1], 1);
            anvil_strbuf_appendf(&be->code, "\tfadd %s0, %s0, %s1\n", reg, reg, reg);
            break;
            
        case ANVIL_OP_FSUB:
            arm64_emit_load_fp_value(be, instr->operands[0], 0);
            arm64_emit_load_fp_value(be, instr->operands[1], 1);
            anvil_strbuf_appendf(&be->code, "\tfsub %s0, %s0, %s1\n", reg, reg, reg);
            break;
            
        case ANVIL_OP_FMUL:
            arm64_emit_load_fp_value(be, instr->operands[0], 0);
            arm64_emit_load_fp_value(be, instr->operands[1], 1);
            anvil_strbuf_appendf(&be->code, "\tfmul %s0, %s0, %s1\n", reg, reg, reg);
            break;
            
        case ANVIL_OP_FDIV:
            arm64_emit_load_fp_value(be, instr->operands[0], 0);
            arm64_emit_load_fp_value(be, instr->operands[1], 1);
            anvil_strbuf_appendf(&be->code, "\tfdiv %s0, %s0, %s1\n", reg, reg, reg);
            break;
            
        case ANVIL_OP_FNEG:
            arm64_emit_load_fp_value(be, instr->operands[0], 0);
            anvil_strbuf_appendf(&be->code, "\tfneg %s0, %s0\n", reg, reg);
            break;
            
        case ANVIL_OP_FABS:
            arm64_emit_load_fp_value(be, instr->operands[0], 0);
            anvil_strbuf_appendf(&be->code, "\tfabs %s0, %s0\n", reg, reg);
            break;
            
        case ANVIL_OP_FCMP:
            arm64_emit_load_fp_value(be, instr->operands[0], 0);
            arm64_emit_load_fp_value(be, instr->operands[1], 1);
            anvil_strbuf_appendf(&be->code, "\tfcmp %s0, %s1\n", reg, reg);
            anvil_strbuf_append(&be->code, "\tcset x0, eq\n");
            break;
            
        case ANVIL_OP_SITOFP:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            if (is_f32) {
                anvil_strbuf_append(&be->code, "\tscvtf s0, x9\n");
                anvil_strbuf_append(&be->code, "\tfmov w0, s0\n");
            } else {
                anvil_strbuf_append(&be->code, "\tscvtf d0, x9\n");
                anvil_strbuf_append(&be->code, "\tfmov x0, d0\n");
            }
            break;
            
        case ANVIL_OP_UITOFP:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            if (is_f32) {
                anvil_strbuf_append(&be->code, "\tucvtf s0, x9\n");
                anvil_strbuf_append(&be->code, "\tfmov w0, s0\n");
            } else {
                anvil_strbuf_append(&be->code, "\tucvtf d0, x9\n");
                anvil_strbuf_append(&be->code, "\tfmov x0, d0\n");
            }
            break;
            
        case ANVIL_OP_FPTOSI:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            if (is_f32) {
                anvil_strbuf_append(&be->code, "\tfmov s0, w9\n");
                anvil_strbuf_append(&be->code, "\tfcvtzs x0, s0\n");
            } else {
                anvil_strbuf_append(&be->code, "\tfmov d0, x9\n");
                anvil_strbuf_append(&be->code, "\tfcvtzs x0, d0\n");
            }
            break;
            
        case ANVIL_OP_FPTOUI:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            if (is_f32) {
                anvil_strbuf_append(&be->code, "\tfmov s0, w9\n");
                anvil_strbuf_append(&be->code, "\tfcvtzu x0, s0\n");
            } else {
                anvil_strbuf_append(&be->code, "\tfmov d0, x9\n");
                anvil_strbuf_append(&be->code, "\tfcvtzu x0, d0\n");
            }
            break;
            
        case ANVIL_OP_FPEXT:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            anvil_strbuf_append(&be->code, "\tfmov s0, w9\n");
            anvil_strbuf_append(&be->code, "\tfcvt d0, s0\n");
            anvil_strbuf_append(&be->code, "\tfmov x0, d0\n");
            break;
            
        case ANVIL_OP_FPTRUNC:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            anvil_strbuf_append(&be->code, "\tfmov d0, x9\n");
            anvil_strbuf_append(&be->code, "\tfcvt s0, d0\n");
            anvil_strbuf_append(&be->code, "\tfmov w0, s0\n");
            break;
            
        default:
            break;
    }
}
