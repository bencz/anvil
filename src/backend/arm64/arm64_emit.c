/*
 * ANVIL - ARM64 Backend - Instruction Emission
 * 
 * Functions for emitting ARM64 instructions from IR.
 */

#include "arm64_internal.h"
#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Register Value Cache
 * ============================================================================ */

/* Check if value is already in a register */
static int arm64_find_cached_value(arm64_backend_t *be, anvil_value_t *val)
{
    if (!val) return -1;
    
    /* Only cache instruction results (stack-allocated values) */
    if (val->kind != ANVIL_VAL_INSTR && val->kind != ANVIL_VAL_PARAM) return -1;
    
    /* Check temporary registers x9-x15 */
    for (int r = ARM64_X9; r <= ARM64_X15; r++) {
        if (be->gpr[r].value == val && !be->gpr[r].is_dirty) {
            return r;
        }
    }
    return -1;
}

/* Mark register as containing a value */
static void arm64_cache_value(arm64_backend_t *be, int reg, anvil_value_t *val)
{
    if (reg < ARM64_X9 || reg > ARM64_X15) return;
    be->gpr[reg].value = val;
    be->gpr[reg].is_dirty = false;
}

/* Invalidate cache for a value (called after store) */
static void arm64_invalidate_cached_value(arm64_backend_t *be, anvil_value_t *val)
{
    if (!val) return;
    for (int r = ARM64_X9; r <= ARM64_X15; r++) {
        if (be->gpr[r].value == val) {
            be->gpr[r].value = NULL;
        }
    }
}

/* Clear all cached values (called at block boundaries) */
static void arm64_clear_reg_cache(arm64_backend_t *be)
{
    for (int r = ARM64_X9; r <= ARM64_X15; r++) {
        be->gpr[r].value = NULL;
        be->gpr[r].is_dirty = false;
    }
}

/* ============================================================================
 * Value Loading
 * ============================================================================ */

void arm64_emit_load_value(arm64_backend_t *be, anvil_value_t *val, int target_reg)
{
    if (!val) return;
    
    /* Check if value is already in a register */
    int cached_reg = arm64_find_cached_value(be, val);
    if (cached_reg >= 0 && cached_reg != target_reg) {
        /* Value already in a register, just move it */
        anvil_strbuf_appendf(&be->code, "\tmov %s, %s\n",
            arm64_xreg_names[target_reg], arm64_xreg_names[cached_reg]);
        arm64_cache_value(be, target_reg, val);
        return;
    }
    
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
    
    /* Cache the loaded value in the target register */
    arm64_cache_value(be, target_reg, val);
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
    
    /* Calculate stack size (16-byte aligned) */
    int stack_size = (be->next_stack_offset + 15) & ~15;
    
    /* Leaf function optimization: skip saving x30 (link register) if no calls are made.
     * We still need x29 (frame pointer) for stack access in most cases. */
    if (be->is_leaf_func && stack_size == 0) {
        /* Minimal leaf function - no stack frame needed at all */
        be->frame.total_size = 0;
        return;
    }
    
    if (be->is_leaf_func) {
        /* Leaf function with stack - save only x29, not x30 */
        anvil_strbuf_append(&be->code, "\tstr x29, [sp, #-16]!\n");
        anvil_strbuf_append(&be->code, "\tmov x29, sp\n");
        
        if (stack_size > 0) {
            if (stack_size <= 4095) {
                anvil_strbuf_appendf(&be->code, "\tsub sp, sp, #%d\n", stack_size);
            } else {
                anvil_strbuf_appendf(&be->code, "\tmov x16, #%d\n", stack_size);
                anvil_strbuf_append(&be->code, "\tsub sp, sp, x16\n");
            }
        }
        be->frame.total_size = stack_size;
        return;
    }
    
    /* Non-leaf function: save frame pointer and link register */
    anvil_strbuf_append(&be->code, "\tstp x29, x30, [sp, #-16]!\n");
    anvil_strbuf_append(&be->code, "\tmov x29, sp\n");
    
    /* Allocate stack space */
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
    
    /* Minimal leaf function - no stack frame */
    if (be->is_leaf_func && stack_size == 0) {
        anvil_strbuf_append(&be->code, "\tret\n");
        return;
    }
    
    /* Leaf function with stack - restore only x29 */
    if (be->is_leaf_func) {
        if (stack_size > 0) {
            if (stack_size <= 4095) {
                anvil_strbuf_appendf(&be->code, "\tadd sp, sp, #%d\n", stack_size);
            } else {
                anvil_strbuf_appendf(&be->code, "\tmov x16, #%d\n", stack_size);
                anvil_strbuf_append(&be->code, "\tadd sp, sp, x16\n");
            }
        }
        anvil_strbuf_append(&be->code, "\tldr x29, [sp], #16\n");
        anvil_strbuf_append(&be->code, "\tret\n");
        return;
    }
    
    /* Non-leaf function: restore stack and x29/x30 */
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
 * Register Size Helpers
 * ============================================================================ */

/* Determine if we should use 32-bit (W) or 64-bit (X) registers for an instruction */
static bool arm64_use_32bit_regs(anvil_instr_t *instr)
{
    if (!instr || !instr->result || !instr->result->type) return false;
    
    int size = arm64_type_size(instr->result->type);
    return size <= 4;
}

/* Get register name with correct size suffix */
static const char *arm64_sized_reg(int reg, bool use_32bit)
{
    if (reg == ARM64_XZR) return use_32bit ? "wzr" : "xzr";
    if (reg < 0 || reg > 30) return "?";
    return use_32bit ? arm64_wreg_names[reg] : arm64_xreg_names[reg];
}

/* Check if value is a small immediate constant (fits in 12-bit unsigned) */
static bool arm64_is_imm12(anvil_value_t *val, int64_t *out_imm)
{
    if (!val || val->kind != ANVIL_VAL_CONST_INT) return false;
    int64_t imm = val->data.i;
    if (imm >= 0 && imm <= 4095) {
        if (out_imm) *out_imm = imm;
        return true;
    }
    return false;
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
             * No need to zero-initialize here - the C code will initialize
             * the variable with an explicit store instruction. */
            break;
            
        /* Arithmetic */
        case ANVIL_OP_ADD: {
            bool w = arm64_use_32bit_regs(instr);
            int64_t imm;
            /* Check if second operand is immediate */
            if (arm64_is_imm12(instr->operands[1], &imm)) {
                arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
                anvil_strbuf_appendf(&be->code, "\tadd %s, %s, #%lld\n",
                    arm64_sized_reg(0, w), arm64_sized_reg(9, w), (long long)imm);
            }
            /* Check if first operand is immediate (commutative) */
            else if (arm64_is_imm12(instr->operands[0], &imm)) {
                arm64_emit_load_value(be, instr->operands[1], ARM64_X9);
                anvil_strbuf_appendf(&be->code, "\tadd %s, %s, #%lld\n",
                    arm64_sized_reg(0, w), arm64_sized_reg(9, w), (long long)imm);
            }
            else {
                arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
                arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
                anvil_strbuf_appendf(&be->code, "\tadd %s, %s, %s\n",
                    arm64_sized_reg(0, w), arm64_sized_reg(9, w), arm64_sized_reg(10, w));
            }
            arm64_save_result(be, instr);
            break;
        }
            
        case ANVIL_OP_SUB: {
            bool w = arm64_use_32bit_regs(instr);
            int64_t imm;
            /* Check if second operand is immediate */
            if (arm64_is_imm12(instr->operands[1], &imm)) {
                arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
                anvil_strbuf_appendf(&be->code, "\tsub %s, %s, #%lld\n",
                    arm64_sized_reg(0, w), arm64_sized_reg(9, w), (long long)imm);
            }
            else {
                arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
                arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
                anvil_strbuf_appendf(&be->code, "\tsub %s, %s, %s\n",
                    arm64_sized_reg(0, w), arm64_sized_reg(9, w), arm64_sized_reg(10, w));
            }
            arm64_save_result(be, instr);
            break;
        }
            
        case ANVIL_OP_MUL: {
            bool w = arm64_use_32bit_regs(instr);
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_appendf(&be->code, "\tmul %s, %s, %s\n",
                arm64_sized_reg(0, w), arm64_sized_reg(9, w), arm64_sized_reg(10, w));
            arm64_save_result(be, instr);
            break;
        }
            
        case ANVIL_OP_SDIV: {
            bool w = arm64_use_32bit_regs(instr);
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_appendf(&be->code, "\tsdiv %s, %s, %s\n",
                arm64_sized_reg(0, w), arm64_sized_reg(9, w), arm64_sized_reg(10, w));
            arm64_save_result(be, instr);
            break;
        }
            
        case ANVIL_OP_UDIV: {
            bool w = arm64_use_32bit_regs(instr);
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_appendf(&be->code, "\tudiv %s, %s, %s\n",
                arm64_sized_reg(0, w), arm64_sized_reg(9, w), arm64_sized_reg(10, w));
            arm64_save_result(be, instr);
            break;
        }
            
        case ANVIL_OP_SMOD: {
            bool w = arm64_use_32bit_regs(instr);
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_appendf(&be->code, "\tsdiv %s, %s, %s\n",
                arm64_sized_reg(11, w), arm64_sized_reg(9, w), arm64_sized_reg(10, w));
            anvil_strbuf_appendf(&be->code, "\tmsub %s, %s, %s, %s\n",
                arm64_sized_reg(0, w), arm64_sized_reg(11, w), arm64_sized_reg(10, w), arm64_sized_reg(9, w));
            arm64_save_result(be, instr);
            break;
        }
            
        case ANVIL_OP_UMOD: {
            bool w = arm64_use_32bit_regs(instr);
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_appendf(&be->code, "\tudiv %s, %s, %s\n",
                arm64_sized_reg(11, w), arm64_sized_reg(9, w), arm64_sized_reg(10, w));
            anvil_strbuf_appendf(&be->code, "\tmsub %s, %s, %s, %s\n",
                arm64_sized_reg(0, w), arm64_sized_reg(11, w), arm64_sized_reg(10, w), arm64_sized_reg(9, w));
            arm64_save_result(be, instr);
            break;
        }
            
        case ANVIL_OP_NEG: {
            bool w = arm64_use_32bit_regs(instr);
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            anvil_strbuf_appendf(&be->code, "\tneg %s, %s\n",
                arm64_sized_reg(0, w), arm64_sized_reg(9, w));
            arm64_save_result(be, instr);
            break;
        }
            
        /* Bitwise */
        case ANVIL_OP_AND: {
            bool w = arm64_use_32bit_regs(instr);
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_appendf(&be->code, "\tand %s, %s, %s\n",
                arm64_sized_reg(0, w), arm64_sized_reg(9, w), arm64_sized_reg(10, w));
            arm64_save_result(be, instr);
            break;
        }
            
        case ANVIL_OP_OR: {
            bool w = arm64_use_32bit_regs(instr);
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_appendf(&be->code, "\torr %s, %s, %s\n",
                arm64_sized_reg(0, w), arm64_sized_reg(9, w), arm64_sized_reg(10, w));
            arm64_save_result(be, instr);
            break;
        }
            
        case ANVIL_OP_XOR: {
            bool w = arm64_use_32bit_regs(instr);
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_appendf(&be->code, "\teor %s, %s, %s\n",
                arm64_sized_reg(0, w), arm64_sized_reg(9, w), arm64_sized_reg(10, w));
            arm64_save_result(be, instr);
            break;
        }
            
        case ANVIL_OP_NOT: {
            bool w = arm64_use_32bit_regs(instr);
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            anvil_strbuf_appendf(&be->code, "\tmvn %s, %s\n",
                arm64_sized_reg(0, w), arm64_sized_reg(9, w));
            arm64_save_result(be, instr);
            break;
        }
            
        case ANVIL_OP_SHL: {
            bool w = arm64_use_32bit_regs(instr);
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_appendf(&be->code, "\tlsl %s, %s, %s\n",
                arm64_sized_reg(0, w), arm64_sized_reg(9, w), arm64_sized_reg(10, w));
            arm64_save_result(be, instr);
            break;
        }
            
        case ANVIL_OP_SHR: {
            bool w = arm64_use_32bit_regs(instr);
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_appendf(&be->code, "\tlsr %s, %s, %s\n",
                arm64_sized_reg(0, w), arm64_sized_reg(9, w), arm64_sized_reg(10, w));
            arm64_save_result(be, instr);
            break;
        }
            
        case ANVIL_OP_SAR: {
            bool w = arm64_use_32bit_regs(instr);
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_appendf(&be->code, "\tasr %s, %s, %s\n",
                arm64_sized_reg(0, w), arm64_sized_reg(9, w), arm64_sized_reg(10, w));
            arm64_save_result(be, instr);
            break;
        }
            
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

/* Check if comparison result is only used by the immediately following BR_COND */
static bool cmp_used_only_by_next_br_cond(anvil_instr_t *cmp_instr)
{
    if (!cmp_instr || !cmp_instr->result) return false;
    
    anvil_instr_t *next = cmp_instr->next;
    if (!next || next->op != ANVIL_OP_BR_COND) return false;
    
    /* Check if BR_COND uses this comparison result */
    if (next->num_operands >= 1 && next->operands[0] == cmp_instr->result) {
        return true;
    }
    
    return false;
}

void arm64_emit_cmp(arm64_backend_t *be, anvil_instr_t *instr)
{
    /* Optimization: if this comparison is immediately followed by BR_COND
     * that uses the result, skip cset and save - BR_COND will emit the
     * fused cmp + b.cond sequence */
    if (cmp_used_only_by_next_br_cond(instr)) {
        /* Don't emit anything - arm64_emit_br_cond will handle it */
        return;
    }
    
    int64_t imm;
    /* Check if second operand is immediate */
    if (arm64_is_imm12(instr->operands[1], &imm)) {
        arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
        anvil_strbuf_appendf(&be->code, "\tcmp x9, #%lld\n", (long long)imm);
    }
    else {
        arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
        arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
        anvil_strbuf_append(&be->code, "\tcmp x9, x10\n");
    }
    
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
    /* Clear register cache at branch (values may not be valid in target block) */
    arm64_clear_reg_cache(be);
    
    if (instr->true_block) {
        arm64_emit_phi_copies(be, instr->parent, instr->true_block);
        anvil_strbuf_appendf(&be->code, "\tb .L%s_%s\n",
            be->current_func->name, instr->true_block->name);
    }
}

/* Get ARM64 condition code for comparison opcode */
static const char *arm64_cond_for_cmp(anvil_op_t op)
{
    switch (op) {
        case ANVIL_OP_CMP_EQ:  return "eq";
        case ANVIL_OP_CMP_NE:  return "ne";
        case ANVIL_OP_CMP_LT:  return "lt";
        case ANVIL_OP_CMP_LE:  return "le";
        case ANVIL_OP_CMP_GT:  return "gt";
        case ANVIL_OP_CMP_GE:  return "ge";
        case ANVIL_OP_CMP_ULT: return "lo";  /* unsigned lower */
        case ANVIL_OP_CMP_ULE: return "ls";  /* unsigned lower or same */
        case ANVIL_OP_CMP_UGT: return "hi";  /* unsigned higher */
        case ANVIL_OP_CMP_UGE: return "hs";  /* unsigned higher or same */
        default: return NULL;
    }
}

/* Check if value is a comparison instruction result */
static anvil_instr_t *get_cmp_instr(anvil_value_t *val)
{
    if (!val) return NULL;
    if (val->kind != ANVIL_VAL_INSTR) return NULL;
    if (!val->data.instr) return NULL;
    
    anvil_op_t op = val->data.instr->op;
    if (op >= ANVIL_OP_CMP_EQ && op <= ANVIL_OP_CMP_UGE) {
        return val->data.instr;
    }
    return NULL;
}

void arm64_emit_br_cond(arm64_backend_t *be, anvil_instr_t *instr)
{
    /* Clear register cache at branch */
    arm64_clear_reg_cache(be);
    
    anvil_value_t *cond = instr->operands[0];
    anvil_instr_t *cmp_instr = get_cmp_instr(cond);
    
    if (instr->true_block && instr->false_block) {
        bool true_has_phi = instr->true_block->first && 
                            instr->true_block->first->op == ANVIL_OP_PHI;
        bool false_has_phi = instr->false_block->first && 
                             instr->false_block->first->op == ANVIL_OP_PHI;
        
        /* Optimization: if condition is a comparison, use b.cond directly */
        if (cmp_instr && !true_has_phi && !false_has_phi) {
            const char *cond_code = arm64_cond_for_cmp(cmp_instr->op);
            if (cond_code) {
                int64_t imm;
                /* Special case: comparison with zero can use cbz/cbnz */
                if (arm64_is_imm12(cmp_instr->operands[1], &imm) && imm == 0) {
                    arm64_emit_load_value(be, cmp_instr->operands[0], ARM64_X9);
                    if (cmp_instr->op == ANVIL_OP_CMP_EQ) {
                        /* x == 0 -> cbz */
                        anvil_strbuf_appendf(&be->code, "\tcbz x9, .L%s_%s\n",
                            be->current_func->name, instr->true_block->name);
                        anvil_strbuf_appendf(&be->code, "\tb .L%s_%s\n",
                            be->current_func->name, instr->false_block->name);
                        return;
                    } else if (cmp_instr->op == ANVIL_OP_CMP_NE) {
                        /* x != 0 -> cbnz */
                        anvil_strbuf_appendf(&be->code, "\tcbnz x9, .L%s_%s\n",
                            be->current_func->name, instr->true_block->name);
                        anvil_strbuf_appendf(&be->code, "\tb .L%s_%s\n",
                            be->current_func->name, instr->false_block->name);
                        return;
                    }
                    /* For other comparisons with 0, use cmp #0 + b.cond */
                    anvil_strbuf_append(&be->code, "\tcmp x9, #0\n");
                } else if (arm64_is_imm12(cmp_instr->operands[1], &imm)) {
                    /* Non-zero immediate */
                    arm64_emit_load_value(be, cmp_instr->operands[0], ARM64_X9);
                    anvil_strbuf_appendf(&be->code, "\tcmp x9, #%lld\n", (long long)imm);
                } else {
                    /* Register comparison */
                    arm64_emit_load_value(be, cmp_instr->operands[0], ARM64_X9);
                    arm64_emit_load_value(be, cmp_instr->operands[1], ARM64_X10);
                    anvil_strbuf_append(&be->code, "\tcmp x9, x10\n");
                }
                anvil_strbuf_appendf(&be->code, "\tb.%s .L%s_%s\n",
                    cond_code, be->current_func->name, instr->true_block->name);
                anvil_strbuf_appendf(&be->code, "\tb .L%s_%s\n",
                    be->current_func->name, instr->false_block->name);
                return;
            }
        }
        
        /* Fallback: load condition value and use cbnz */
        arm64_emit_load_value(be, cond, ARM64_X9);
        
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
    /* Clear register cache - call clobbers caller-saved registers */
    arm64_clear_reg_cache(be);
    
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
