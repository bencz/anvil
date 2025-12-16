/*
 * ANVIL - PowerPC 64-bit Backend Instruction Emission
 * 
 * This module handles the emission of PPC64 assembly instructions.
 */

#include "ppc64_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Prologue/Epilogue Emission
 * ============================================================================ */

void ppc64_emit_prologue(ppc64_backend_t *be, anvil_func_t *func)
{
    size_t frame_size = func->stack_size;
    if (frame_size < PPC64_MIN_FRAME_SIZE) frame_size = PPC64_MIN_FRAME_SIZE;
    frame_size = (frame_size + 15) & ~15; /* Align to 16 bytes */
    
    /* Function descriptor (ELFv1 ABI) */
    anvil_strbuf_appendf(&be->code, "\t.section \".opd\",\"aw\"\n");
    anvil_strbuf_appendf(&be->code, "\t.align 3\n");
    anvil_strbuf_appendf(&be->code, "\t.globl %s\n", func->name);
    anvil_strbuf_appendf(&be->code, "%s:\n", func->name);
    anvil_strbuf_appendf(&be->code, "\t.quad .L.%s,.TOC.@tocbase,0\n", func->name);
    anvil_strbuf_appendf(&be->code, "\t.previous\n");
    anvil_strbuf_appendf(&be->code, "\t.type %s, @function\n", func->name);
    
    /* Actual function code */
    anvil_strbuf_appendf(&be->code, ".L.%s:\n", func->name);
    
    /* Save link register */
    anvil_strbuf_append(&be->code, "\tmflr r0\n");
    anvil_strbuf_appendf(&be->code, "\tstd r0, %d(r1)\n", PPC64_LR_SAVE_OFFSET);
    
    /* Save TOC pointer */
    anvil_strbuf_appendf(&be->code, "\tstd r2, %d(r1)\n", PPC64_TOC_SAVE_OFFSET);
    
    /* Save callee-saved registers if needed */
    anvil_strbuf_append(&be->code, "\tstd r31, -8(r1)\n");
    
    /* Create stack frame */
    anvil_strbuf_appendf(&be->code, "\tstdu r1, -%zu(r1)\n", frame_size);
    
    /* Set up frame pointer */
    anvil_strbuf_appendf(&be->code, "\taddi r31, r1, %zu\n", frame_size);
    
    be->local_offset = PPC64_MIN_FRAME_SIZE;
}

void ppc64_emit_epilogue(ppc64_backend_t *be, anvil_func_t *func)
{
    size_t frame_size = func->stack_size;
    if (frame_size < PPC64_MIN_FRAME_SIZE) frame_size = PPC64_MIN_FRAME_SIZE;
    frame_size = (frame_size + 15) & ~15;
    
    /* Restore stack pointer */
    anvil_strbuf_appendf(&be->code, "\taddi r1, r1, %zu\n", frame_size);
    
    /* Restore callee-saved registers */
    anvil_strbuf_append(&be->code, "\tld r31, -8(r1)\n");
    
    /* Restore TOC pointer */
    anvil_strbuf_appendf(&be->code, "\tld r2, %d(r1)\n", PPC64_TOC_SAVE_OFFSET);
    
    /* Restore link register and return */
    anvil_strbuf_appendf(&be->code, "\tld r0, %d(r1)\n", PPC64_LR_SAVE_OFFSET);
    anvil_strbuf_append(&be->code, "\tmtlr r0\n");
    anvil_strbuf_append(&be->code, "\tblr\n");
}

/* ============================================================================
 * Value Loading
 * ============================================================================ */

void ppc64_emit_load_value(ppc64_backend_t *be, anvil_value_t *val, int reg, anvil_func_t *func)
{
    if (!val) return;
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_INT:
            if (val->data.i >= -32768 && val->data.i <= 32767) {
                anvil_strbuf_appendf(&be->code, "\tli %s, %lld\n",
                    ppc64_gpr_names[reg], (long long)val->data.i);
            } else if (val->data.i >= -2147483648LL && val->data.i <= 2147483647LL) {
                /* 32-bit immediate */
                anvil_strbuf_appendf(&be->code, "\tlis %s, %lld@ha\n",
                    ppc64_gpr_names[reg], (long long)((val->data.i >> 16) & 0xFFFF));
                anvil_strbuf_appendf(&be->code, "\tori %s, %s, %lld@l\n",
                    ppc64_gpr_names[reg], ppc64_gpr_names[reg], 
                    (long long)(val->data.i & 0xFFFF));
            } else {
                /* Full 64-bit immediate - load in parts */
                int64_t v = val->data.i;
                anvil_strbuf_appendf(&be->code, "\tlis %s, %lld\n",
                    ppc64_gpr_names[reg], (long long)((v >> 48) & 0xFFFF));
                anvil_strbuf_appendf(&be->code, "\tori %s, %s, %lld\n",
                    ppc64_gpr_names[reg], ppc64_gpr_names[reg], 
                    (long long)((v >> 32) & 0xFFFF));
                anvil_strbuf_appendf(&be->code, "\tsldi %s, %s, 32\n",
                    ppc64_gpr_names[reg], ppc64_gpr_names[reg]);
                anvil_strbuf_appendf(&be->code, "\toris %s, %s, %lld\n",
                    ppc64_gpr_names[reg], ppc64_gpr_names[reg],
                    (long long)((v >> 16) & 0xFFFF));
                anvil_strbuf_appendf(&be->code, "\tori %s, %s, %lld\n",
                    ppc64_gpr_names[reg], ppc64_gpr_names[reg],
                    (long long)(v & 0xFFFF));
            }
            break;
            
        case ANVIL_VAL_PARAM:
            {
                size_t idx = val->data.param.index;
                if (idx < PPC64_NUM_ARG_REGS) {
                    if (ppc64_arg_regs[idx] != reg) {
                        anvil_strbuf_appendf(&be->code, "\tmr %s, %s\n",
                            ppc64_gpr_names[reg], ppc64_gpr_names[ppc64_arg_regs[idx]]);
                    }
                } else {
                    size_t offset = PPC64_PARAM_SAVE_OFFSET + (idx - PPC64_NUM_ARG_REGS) * 8;
                    anvil_strbuf_appendf(&be->code, "\tld %s, %zu(r31)\n",
                        ppc64_gpr_names[reg], offset);
                }
            }
            break;
            
        case ANVIL_VAL_CONST_NULL:
            anvil_strbuf_appendf(&be->code, "\tli %s, 0\n", ppc64_gpr_names[reg]);
            break;
            
        case ANVIL_VAL_CONST_STRING:
            {
                const char *label = ppc64_add_string(be, val->data.str ? val->data.str : "");
                anvil_strbuf_appendf(&be->code, "\taddis %s, r2, %s@toc@ha\n",
                    ppc64_gpr_names[reg], label);
                anvil_strbuf_appendf(&be->code, "\taddi %s, %s, %s@toc@l\n",
                    ppc64_gpr_names[reg], ppc64_gpr_names[reg], label);
            }
            break;
            
        case ANVIL_VAL_INSTR:
            if (val->data.instr && val->data.instr->op == ANVIL_OP_ALLOCA) {
                /* Load address of stack slot */
                int offset = ppc64_get_stack_slot(be, val);
                if (offset >= 0) {
                    anvil_strbuf_appendf(&be->code, "\taddi %s, r31, -%d\n",
                        ppc64_gpr_names[reg], PPC64_MIN_FRAME_SIZE + offset);
                }
            } else {
                /* Result should be in r3 by convention */
                if (reg != PPC64_R3) {
                    anvil_strbuf_appendf(&be->code, "\tmr %s, r3\n", ppc64_gpr_names[reg]);
                }
            }
            break;
            
        case ANVIL_VAL_FUNC:
            /* Load function address via TOC */
            anvil_strbuf_appendf(&be->code, "\taddis %s, r2, %s@toc@ha\n",
                ppc64_gpr_names[reg], val->name);
            anvil_strbuf_appendf(&be->code, "\tld %s, %s@toc@l(%s)\n",
                ppc64_gpr_names[reg], val->name, ppc64_gpr_names[reg]);
            break;
            
        case ANVIL_VAL_GLOBAL:
            anvil_strbuf_appendf(&be->code, "\taddis %s, r2, %s@toc@ha\n",
                ppc64_gpr_names[reg], val->name);
            anvil_strbuf_appendf(&be->code, "\tld %s, %s@toc@l(%s)\n",
                ppc64_gpr_names[reg], val->name, ppc64_gpr_names[reg]);
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "\t# unhandled value kind %d\n", val->kind);
            break;
    }
    
    (void)func;
}

/* ============================================================================
 * Instruction Emission
 * ============================================================================ */

void ppc64_emit_instr(ppc64_backend_t *be, anvil_instr_t *instr, anvil_func_t *func)
{
    if (!instr) return;
    
    switch (instr->op) {
        case ANVIL_OP_ADD:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
            anvil_strbuf_append(&be->code, "\tadd r3, r3, r4\n");
            break;
            
        case ANVIL_OP_SUB:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
            anvil_strbuf_append(&be->code, "\tsub r3, r3, r4\n");
            break;
            
        case ANVIL_OP_MUL:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
            anvil_strbuf_append(&be->code, "\tmulld r3, r3, r4\n");
            break;
            
        case ANVIL_OP_SDIV:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
            anvil_strbuf_append(&be->code, "\tdivd r3, r3, r4\n");
            break;
            
        case ANVIL_OP_UDIV:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
            anvil_strbuf_append(&be->code, "\tdivdu r3, r3, r4\n");
            break;
            
        case ANVIL_OP_SMOD:
        case ANVIL_OP_UMOD:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
            if (instr->op == ANVIL_OP_SMOD) {
                anvil_strbuf_append(&be->code, "\tdivd r5, r3, r4\n");
            } else {
                anvil_strbuf_append(&be->code, "\tdivdu r5, r3, r4\n");
            }
            anvil_strbuf_append(&be->code, "\tmulld r5, r5, r4\n");
            anvil_strbuf_append(&be->code, "\tsub r3, r3, r5\n");
            break;
            
        case ANVIL_OP_NEG:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            anvil_strbuf_append(&be->code, "\tneg r3, r3\n");
            break;
            
        case ANVIL_OP_AND:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
            anvil_strbuf_append(&be->code, "\tand r3, r3, r4\n");
            break;
            
        case ANVIL_OP_OR:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
            anvil_strbuf_append(&be->code, "\tor r3, r3, r4\n");
            break;
            
        case ANVIL_OP_XOR:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
            anvil_strbuf_append(&be->code, "\txor r3, r3, r4\n");
            break;
            
        case ANVIL_OP_NOT:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            anvil_strbuf_append(&be->code, "\tnot r3, r3\n");
            break;
            
        case ANVIL_OP_SHL:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
            anvil_strbuf_append(&be->code, "\tsld r3, r3, r4\n");
            break;
            
        case ANVIL_OP_SHR:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
            anvil_strbuf_append(&be->code, "\tsrd r3, r3, r4\n");
            break;
            
        case ANVIL_OP_SAR:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
            anvil_strbuf_append(&be->code, "\tsrad r3, r3, r4\n");
            break;
            
        case ANVIL_OP_PHI:
            /* PHI nodes handled during SSA resolution */
            break;
            
        case ANVIL_OP_ALLOCA:
            {
                int offset = ppc64_add_stack_slot(be, instr->result);
                /* Zero-initialize the slot */
                anvil_strbuf_append(&be->code, "\tli r0, 0\n");
                anvil_strbuf_appendf(&be->code, "\tstd r0, -%d(r31)\n", PPC64_MIN_FRAME_SIZE + offset);
            }
            break;
            
        case ANVIL_OP_LOAD:
            /* Check if loading from stack slot */
            if (instr->operands[0]->kind == ANVIL_VAL_INSTR &&
                instr->operands[0]->data.instr &&
                instr->operands[0]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = ppc64_get_stack_slot(be, instr->operands[0]);
                if (offset >= 0) {
                    anvil_strbuf_appendf(&be->code, "\tld r3, -%d(r31)\n", PPC64_MIN_FRAME_SIZE + offset);
                    break;
                }
            }
            /* Check if loading from global */
            if (instr->operands[0]->kind == ANVIL_VAL_GLOBAL) {
                anvil_strbuf_appendf(&be->code, "\taddis r4, r2, %s@toc@ha\n", instr->operands[0]->name);
                anvil_strbuf_appendf(&be->code, "\tld r3, %s@toc@l(r4)\n", instr->operands[0]->name);
                break;
            }
            /* Generic load */
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R4, func);
            anvil_strbuf_append(&be->code, "\tld r3, 0(r4)\n");
            break;
            
        case ANVIL_OP_STORE:
            /* Check if storing to stack slot */
            if (instr->operands[1]->kind == ANVIL_VAL_INSTR &&
                instr->operands[1]->data.instr &&
                instr->operands[1]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = ppc64_get_stack_slot(be, instr->operands[1]);
                if (offset >= 0) {
                    ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
                    anvil_strbuf_appendf(&be->code, "\tstd r3, -%d(r31)\n", PPC64_MIN_FRAME_SIZE + offset);
                    break;
                }
            }
            /* Check if storing to global */
            if (instr->operands[1]->kind == ANVIL_VAL_GLOBAL) {
                ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
                anvil_strbuf_appendf(&be->code, "\taddis r4, r2, %s@toc@ha\n", instr->operands[1]->name);
                anvil_strbuf_appendf(&be->code, "\tstd r3, %s@toc@l(r4)\n", instr->operands[1]->name);
                break;
            }
            /* Generic store */
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
            anvil_strbuf_append(&be->code, "\tstd r3, 0(r4)\n");
            break;
            
        case ANVIL_OP_GEP:
            /* Get Element Pointer - array indexing */
            {
                ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
                
                if (instr->num_operands > 1) {
                    ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
                    
                    int elem_size = 8;
                    if (instr->result && instr->result->type) {
                        anvil_type_t *ptr_type = instr->result->type;
                        if (ptr_type->kind == ANVIL_TYPE_PTR && ptr_type->data.pointee) {
                            anvil_type_t *elem_type = ptr_type->data.pointee;
                            switch (elem_type->kind) {
                                case ANVIL_TYPE_I8:
                                case ANVIL_TYPE_U8:
                                    elem_size = 1;
                                    break;
                                case ANVIL_TYPE_I16:
                                case ANVIL_TYPE_U16:
                                    elem_size = 2;
                                    break;
                                case ANVIL_TYPE_I32:
                                case ANVIL_TYPE_U32:
                                case ANVIL_TYPE_F32:
                                    elem_size = 4;
                                    break;
                                default:
                                    elem_size = 8;
                                    break;
                            }
                        }
                    }
                    
                    if (elem_size == 1) {
                        anvil_strbuf_append(&be->code, "\tadd r3, r3, r4\n");
                    } else if (elem_size == 2) {
                        anvil_strbuf_append(&be->code, "\tsldi r4, r4, 1\n");
                        anvil_strbuf_append(&be->code, "\tadd r3, r3, r4\n");
                    } else if (elem_size == 4) {
                        anvil_strbuf_append(&be->code, "\tsldi r4, r4, 2\n");
                        anvil_strbuf_append(&be->code, "\tadd r3, r3, r4\n");
                    } else {
                        anvil_strbuf_append(&be->code, "\tsldi r4, r4, 3\n");
                        anvil_strbuf_append(&be->code, "\tadd r3, r3, r4\n");
                    }
                }
            }
            break;
            
        case ANVIL_OP_STRUCT_GEP:
            /* Get Struct Field Pointer */
            {
                ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
                
                int offset = 0;
                if (instr->aux_type && instr->aux_type->kind == ANVIL_TYPE_STRUCT &&
                    instr->num_operands > 1 && instr->operands[1]->kind == ANVIL_VAL_CONST_INT) {
                    unsigned field_idx = (unsigned)instr->operands[1]->data.i;
                    if (field_idx < instr->aux_type->data.struc.num_fields) {
                        offset = (int)instr->aux_type->data.struc.offsets[field_idx];
                    }
                }
                
                if (offset != 0) {
                    if (offset >= -32768 && offset <= 32767) {
                        anvil_strbuf_appendf(&be->code, "\taddi r3, r3, %d\n", offset);
                    } else {
                        anvil_strbuf_appendf(&be->code, "\tlis r4, %d@ha\n", offset);
                        anvil_strbuf_appendf(&be->code, "\taddi r4, r4, %d@l\n", offset);
                        anvil_strbuf_append(&be->code, "\tadd r3, r3, r4\n");
                    }
                }
            }
            break;
            
        case ANVIL_OP_BR:
            anvil_strbuf_appendf(&be->code, "\tb .L%s_%s\n", func->name, instr->true_block->name);
            break;
            
        case ANVIL_OP_BR_COND:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            anvil_strbuf_append(&be->code, "\tcmpdi cr0, r3, 0\n");
            anvil_strbuf_appendf(&be->code, "\tbne cr0, .L%s_%s\n", func->name, instr->true_block->name);
            anvil_strbuf_appendf(&be->code, "\tb .L%s_%s\n", func->name, instr->false_block->name);
            break;
            
        case ANVIL_OP_RET:
            if (instr->num_operands > 0) {
                ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            }
            ppc64_emit_epilogue(be, func);
            break;
            
        case ANVIL_OP_CALL:
            for (size_t i = 1; i < instr->num_operands && i <= PPC64_NUM_ARG_REGS; i++) {
                ppc64_emit_load_value(be, instr->operands[i], ppc64_arg_regs[i-1], func);
            }
            /* Save TOC, call, restore TOC */
            anvil_strbuf_appendf(&be->code, "\tstd r2, %d(r1)\n", PPC64_TOC_SAVE_OFFSET);
            anvil_strbuf_appendf(&be->code, "\tbl %s\n", instr->operands[0]->name);
            anvil_strbuf_append(&be->code, "\tnop\n"); /* TOC restore hint */
            anvil_strbuf_appendf(&be->code, "\tld r2, %d(r1)\n", PPC64_TOC_SAVE_OFFSET);
            break;
            
        case ANVIL_OP_CMP_EQ:
        case ANVIL_OP_CMP_NE:
        case ANVIL_OP_CMP_LT:
        case ANVIL_OP_CMP_LE:
        case ANVIL_OP_CMP_GT:
        case ANVIL_OP_CMP_GE:
            {
                ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
                ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
                anvil_strbuf_append(&be->code, "\tcmpd cr0, r3, r4\n");
                
                anvil_strbuf_append(&be->code, "\tli r3, 0\n");
                
                const char *cond;
                switch (instr->op) {
                    case ANVIL_OP_CMP_EQ: cond = "eq"; break;
                    case ANVIL_OP_CMP_NE: cond = "ne"; break;
                    case ANVIL_OP_CMP_LT: cond = "lt"; break;
                    case ANVIL_OP_CMP_LE: cond = "le"; break;
                    case ANVIL_OP_CMP_GT: cond = "gt"; break;
                    case ANVIL_OP_CMP_GE: cond = "ge"; break;
                    default: cond = "eq"; break;
                }
                
                int skip_label = be->label_counter++;
                anvil_strbuf_appendf(&be->code, "\tb%s cr0, .Lskip%d\n", cond, skip_label);
                anvil_strbuf_append(&be->code, "\tli r3, 1\n");
                anvil_strbuf_appendf(&be->code, ".Lskip%d:\n", skip_label);
            }
            break;
            
        case ANVIL_OP_CMP_ULT:
        case ANVIL_OP_CMP_ULE:
        case ANVIL_OP_CMP_UGT:
        case ANVIL_OP_CMP_UGE:
            {
                ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
                ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
                anvil_strbuf_append(&be->code, "\tcmpld cr0, r3, r4\n");
                
                anvil_strbuf_append(&be->code, "\tli r3, 0\n");
                
                const char *cond;
                switch (instr->op) {
                    case ANVIL_OP_CMP_ULT: cond = "lt"; break;
                    case ANVIL_OP_CMP_ULE: cond = "le"; break;
                    case ANVIL_OP_CMP_UGT: cond = "gt"; break;
                    case ANVIL_OP_CMP_UGE: cond = "ge"; break;
                    default: cond = "eq"; break;
                }
                
                int skip_label = be->label_counter++;
                anvil_strbuf_appendf(&be->code, "\tb%s cr0, .Lskip%d\n", cond, skip_label);
                anvil_strbuf_append(&be->code, "\tli r3, 1\n");
                anvil_strbuf_appendf(&be->code, ".Lskip%d:\n", skip_label);
            }
            break;
            
        case ANVIL_OP_TRUNC:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            /* Truncation - mask to target size */
            if (instr->result && instr->result->type) {
                switch (instr->result->type->kind) {
                    case ANVIL_TYPE_I8:
                    case ANVIL_TYPE_U8:
                        anvil_strbuf_append(&be->code, "\trldicl r3, r3, 0, 56\n");
                        break;
                    case ANVIL_TYPE_I16:
                    case ANVIL_TYPE_U16:
                        anvil_strbuf_append(&be->code, "\trldicl r3, r3, 0, 48\n");
                        break;
                    case ANVIL_TYPE_I32:
                    case ANVIL_TYPE_U32:
                        anvil_strbuf_append(&be->code, "\trldicl r3, r3, 0, 32\n");
                        break;
                    default:
                        break;
                }
            }
            break;
            
        case ANVIL_OP_ZEXT:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            /* Zero extension - clear high bits */
            if (instr->operands[0]->type) {
                switch (instr->operands[0]->type->kind) {
                    case ANVIL_TYPE_I8:
                    case ANVIL_TYPE_U8:
                        anvil_strbuf_append(&be->code, "\trldicl r3, r3, 0, 56\n");
                        break;
                    case ANVIL_TYPE_I16:
                    case ANVIL_TYPE_U16:
                        anvil_strbuf_append(&be->code, "\trldicl r3, r3, 0, 48\n");
                        break;
                    case ANVIL_TYPE_I32:
                    case ANVIL_TYPE_U32:
                        anvil_strbuf_append(&be->code, "\trldicl r3, r3, 0, 32\n");
                        break;
                    default:
                        break;
                }
            }
            break;
            
        case ANVIL_OP_SEXT:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            /* Sign extension */
            if (instr->operands[0]->type) {
                switch (instr->operands[0]->type->kind) {
                    case ANVIL_TYPE_I8:
                        anvil_strbuf_append(&be->code, "\textsb r3, r3\n");
                        break;
                    case ANVIL_TYPE_I16:
                        anvil_strbuf_append(&be->code, "\textsh r3, r3\n");
                        break;
                    case ANVIL_TYPE_I32:
                        anvil_strbuf_append(&be->code, "\textsw r3, r3\n");
                        break;
                    default:
                        break;
                }
            }
            break;
            
        case ANVIL_OP_BITCAST:
        case ANVIL_OP_PTRTOINT:
        case ANVIL_OP_INTTOPTR:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            break;
            
        case ANVIL_OP_SELECT:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            ppc64_emit_load_value(be, instr->operands[1], PPC64_R4, func);
            ppc64_emit_load_value(be, instr->operands[2], PPC64_R5, func);
            anvil_strbuf_append(&be->code, "\tcmpdi cr0, r3, 0\n");
            
            /* Use isel if available (POWER7+), otherwise use branch */
            if (ppc64_has_feature(be, ANVIL_FEATURE_PPC_ISEL)) {
                /* isel rd, ra, rb, cr_bit - selects ra if cr_bit is set, rb otherwise */
                anvil_strbuf_append(&be->code, "\tisel r3, r4, r5, 2\n");  /* CR0[EQ] is bit 2 */
            } else {
                int skip_label = be->label_counter++;
                anvil_strbuf_appendf(&be->code, "\tbne cr0, .Lsel%d\n", skip_label);
                anvil_strbuf_append(&be->code, "\tmr r4, r5\n");
                anvil_strbuf_appendf(&be->code, ".Lsel%d:\n", skip_label);
                anvil_strbuf_append(&be->code, "\tmr r3, r4\n");
            }
            break;
            
        /* Floating-point operations (IEEE 754) */
        case ANVIL_OP_FADD:
            anvil_strbuf_append(&be->code, "\t# FP add - load operands to f1, f2\n");
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfadds f1, f1, f2\n");
            } else {
                anvil_strbuf_append(&be->code, "\tfadd f1, f1, f2\n");
            }
            break;
            
        case ANVIL_OP_FSUB:
            anvil_strbuf_append(&be->code, "\t# FP sub - load operands to f1, f2\n");
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfsubs f1, f1, f2\n");
            } else {
                anvil_strbuf_append(&be->code, "\tfsub f1, f1, f2\n");
            }
            break;
            
        case ANVIL_OP_FMUL:
            anvil_strbuf_append(&be->code, "\t# FP mul - load operands to f1, f2\n");
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfmuls f1, f1, f2\n");
            } else {
                anvil_strbuf_append(&be->code, "\tfmul f1, f1, f2\n");
            }
            break;
            
        case ANVIL_OP_FDIV:
            anvil_strbuf_append(&be->code, "\t# FP div - load operands to f1, f2\n");
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfdivs f1, f1, f2\n");
            } else {
                anvil_strbuf_append(&be->code, "\tfdiv f1, f1, f2\n");
            }
            break;
            
        case ANVIL_OP_FNEG:
            anvil_strbuf_append(&be->code, "\tfneg f1, f1\n");
            break;
            
        case ANVIL_OP_FABS:
            anvil_strbuf_append(&be->code, "\tfabs f1, f1\n");
            break;
            
        case ANVIL_OP_FCMP:
            anvil_strbuf_append(&be->code, "\tfcmpu cr0, f1, f2\n");
            anvil_strbuf_append(&be->code, "\tli r3, 1\n");
            {
                int skip_label = be->label_counter++;
                anvil_strbuf_appendf(&be->code, "\tbeq cr0, .Lfcmp%d\n", skip_label);
                anvil_strbuf_append(&be->code, "\tli r3, 0\n");
                anvil_strbuf_appendf(&be->code, ".Lfcmp%d:\n", skip_label);
            }
            break;
            
        case ANVIL_OP_SITOFP:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            anvil_strbuf_append(&be->code, "\tstd r3, -8(r1)\n");
            anvil_strbuf_append(&be->code, "\tlfd f1, -8(r1)\n");
            anvil_strbuf_append(&be->code, "\tfcfid f1, f1\n");
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfrsp f1, f1\n");
            }
            break;
            
        case ANVIL_OP_UITOFP:
            ppc64_emit_load_value(be, instr->operands[0], PPC64_R3, func);
            anvil_strbuf_append(&be->code, "\tstd r3, -8(r1)\n");
            anvil_strbuf_append(&be->code, "\tlfd f1, -8(r1)\n");
            anvil_strbuf_append(&be->code, "\tfcfidu f1, f1\n");
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfrsp f1, f1\n");
            }
            break;
            
        case ANVIL_OP_FPTOSI:
            anvil_strbuf_append(&be->code, "\tfctidz f1, f1\n");
            anvil_strbuf_append(&be->code, "\tstfd f1, -8(r1)\n");
            anvil_strbuf_append(&be->code, "\tld r3, -8(r1)\n");
            break;
            
        case ANVIL_OP_FPTOUI:
            anvil_strbuf_append(&be->code, "\tfctiduz f1, f1\n");
            anvil_strbuf_append(&be->code, "\tstfd f1, -8(r1)\n");
            anvil_strbuf_append(&be->code, "\tld r3, -8(r1)\n");
            break;
            
        case ANVIL_OP_FPEXT:
            /* float to double - PPC FPRs are 64-bit, no conversion needed */
            break;
            
        case ANVIL_OP_FPTRUNC:
            /* double to float */
            anvil_strbuf_append(&be->code, "\tfrsp f1, f1\n");
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "\t# unimplemented op %d\n", instr->op);
            break;
    }
}

/* ============================================================================
 * Block and Function Emission
 * ============================================================================ */

void ppc64_emit_block(ppc64_backend_t *be, anvil_block_t *block, anvil_func_t *func)
{
    if (!block) return;
    
    /* Emit label (skip for entry block) */
    if (block != func->blocks) {
        anvil_strbuf_appendf(&be->code, ".L%s_%s:\n", func->name, block->name);
    }
    
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        ppc64_emit_instr(be, instr, func);
    }
}

void ppc64_emit_func(ppc64_backend_t *be, anvil_func_t *func)
{
    if (!func || func->is_declaration) return;
    
    be->current_func = func;
    be->num_stack_slots = 0;
    be->next_stack_offset = 0;
    
    /* First pass: count stack slots needed */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_ALLOCA) {
                ppc64_add_stack_slot(be, instr->result);
            }
        }
    }
    
    /* Calculate stack size */
    func->stack_size = PPC64_MIN_FRAME_SIZE + be->next_stack_offset + 64;
    
    /* Reset for actual emission */
    be->num_stack_slots = 0;
    
    ppc64_emit_prologue(be, func);
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        ppc64_emit_block(be, block, func);
    }
    
    anvil_strbuf_appendf(&be->code, "\t.size %s, .-.L.%s\n\n", func->name, func->name);
}

/* ============================================================================
 * Global Variables and Strings
 * ============================================================================ */

void ppc64_emit_globals(ppc64_backend_t *be, anvil_module_t *mod)
{
    if (mod->num_globals == 0) return;
    
    anvil_strbuf_append(&be->data, "\t.data\n");
    
    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        anvil_strbuf_appendf(&be->data, "\t.globl %s\n", g->value->name);
        
        int size = 8;
        int align = 8;
        anvil_type_t *type = g->value->type;
        
        if (type) {
            switch (type->kind) {
                case ANVIL_TYPE_I8:
                case ANVIL_TYPE_U8:
                    size = 1; align = 1;
                    break;
                case ANVIL_TYPE_I16:
                case ANVIL_TYPE_U16:
                    size = 2; align = 2;
                    break;
                case ANVIL_TYPE_I32:
                case ANVIL_TYPE_U32:
                case ANVIL_TYPE_F32:
                    size = 4; align = 4;
                    break;
                default:
                    size = 8; align = 8;
                    break;
            }
        }
        
        anvil_strbuf_appendf(&be->data, "\t.align %d\n", align);
        anvil_strbuf_appendf(&be->data, "%s:\n", g->value->name);
        
        if (g->value->data.global.init) {
            anvil_value_t *init = g->value->data.global.init;
            if (init->kind == ANVIL_VAL_CONST_INT) {
                if (size == 1) {
                    anvil_strbuf_appendf(&be->data, "\t.byte %lld\n", (long long)init->data.i);
                } else if (size == 2) {
                    anvil_strbuf_appendf(&be->data, "\t.short %lld\n", (long long)init->data.i);
                } else if (size == 4) {
                    anvil_strbuf_appendf(&be->data, "\t.long %lld\n", (long long)init->data.i);
                } else {
                    anvil_strbuf_appendf(&be->data, "\t.quad %lld\n", (long long)init->data.i);
                }
            } else {
                anvil_strbuf_appendf(&be->data, "\t.zero %d\n", size);
            }
        } else {
            anvil_strbuf_appendf(&be->data, "\t.zero %d\n", size);
        }
    }
    
    anvil_strbuf_append(&be->data, "\n");
}

void ppc64_emit_strings(ppc64_backend_t *be)
{
    if (be->num_strings == 0) return;
    
    anvil_strbuf_append(&be->data, "\t.section .rodata\n");
    
    for (size_t i = 0; i < be->num_strings; i++) {
        ppc64_string_entry_t *entry = &be->strings[i];
        anvil_strbuf_appendf(&be->data, "%s:\n", entry->label);
        anvil_strbuf_append(&be->data, "\t.asciz \"");
        
        for (const char *p = entry->str; *p; p++) {
            switch (*p) {
                case '\n': anvil_strbuf_append(&be->data, "\\n"); break;
                case '\r': anvil_strbuf_append(&be->data, "\\r"); break;
                case '\t': anvil_strbuf_append(&be->data, "\\t"); break;
                case '\\': anvil_strbuf_append(&be->data, "\\\\"); break;
                case '"':  anvil_strbuf_append(&be->data, "\\\""); break;
                default:   anvil_strbuf_append_char(&be->data, *p); break;
            }
        }
        anvil_strbuf_append(&be->data, "\"\n");
    }
    
    anvil_strbuf_append(&be->data, "\n");
}
