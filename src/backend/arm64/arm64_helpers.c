/*
 * ANVIL - ARM64 Backend Helper Functions
 * 
 * Implementation of helper functions for the ARM64 code generator.
 */

#include "arm64_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Register Name Tables
 * ============================================================================ */

const char *arm64_xreg_names[33] = {
    "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
    "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
    "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
    "x24", "x25", "x26", "x27", "x28", "x29", "x30", "sp", "xzr"
};

const char *arm64_wreg_names[33] = {
    "w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7",
    "w8", "w9", "w10", "w11", "w12", "w13", "w14", "w15",
    "w16", "w17", "w18", "w19", "w20", "w21", "w22", "w23",
    "w24", "w25", "w26", "w27", "w28", "w29", "w30", "wsp", "wzr"
};

const char *arm64_dreg_names[32] = {
    "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
    "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15",
    "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23",
    "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31"
};

const char *arm64_sreg_names[32] = {
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15",
    "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
    "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31"
};

/* ============================================================================
 * Register Name Helper
 * ============================================================================ */

const char *arm64_reg_name(int reg, int size, int reg_class)
{
    if (reg_class == ARM64_REG_CLASS_FPR) {
        if (reg < 0 || reg >= 32) return "?fpr";
        return (size <= 4) ? arm64_sreg_names[reg] : arm64_dreg_names[reg];
    }
    
    /* GPR */
    if (reg == ARM64_XZR) {
        return (size <= 4) ? "wzr" : "xzr";
    }
    if (reg == ARM64_SP) {
        return (size <= 4) ? "wsp" : "sp";
    }
    if (reg < 0 || reg > 30) return "?gpr";
    return (size <= 4) ? arm64_wreg_names[reg] : arm64_xreg_names[reg];
}

/* ============================================================================
 * Type Helpers
 * ============================================================================ */

int arm64_type_size(anvil_type_t *type)
{
    if (!type) return 8;
    
    switch (type->kind) {
        case ANVIL_TYPE_VOID:
            return 0;
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_U8:
            return 1;
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_U16:
            return 2;
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_U32:
        case ANVIL_TYPE_F32:
            return 4;
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U64:
        case ANVIL_TYPE_F64:
        case ANVIL_TYPE_PTR:
            return 8;
        case ANVIL_TYPE_ARRAY:
            return type->data.array.count * arm64_type_size(type->data.array.elem);
        case ANVIL_TYPE_STRUCT:
            {
                int size = 0;
                for (size_t i = 0; i < type->data.struc.num_fields; i++) {
                    int field_size = arm64_type_size(type->data.struc.fields[i]);
                    int field_align = arm64_type_align(type->data.struc.fields[i]);
                    /* Align field */
                    size = (size + field_align - 1) & ~(field_align - 1);
                    size += field_size;
                }
                /* Align struct to its alignment */
                int struct_align = arm64_type_align(type);
                size = (size + struct_align - 1) & ~(struct_align - 1);
                return size > 0 ? size : 8;
            }
        case ANVIL_TYPE_FUNC:
            return 8;  /* Function pointer */
        default:
            return 8;
    }
}

int arm64_type_align(anvil_type_t *type)
{
    if (!type) return 8;
    
    switch (type->kind) {
        case ANVIL_TYPE_VOID:
            return 1;
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_U8:
            return 1;
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_U16:
            return 2;
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_U32:
        case ANVIL_TYPE_F32:
            return 4;
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U64:
        case ANVIL_TYPE_F64:
        case ANVIL_TYPE_PTR:
            return 8;
        case ANVIL_TYPE_ARRAY:
            return arm64_type_align(type->data.array.elem);
        case ANVIL_TYPE_STRUCT:
            {
                int max_align = 1;
                for (size_t i = 0; i < type->data.struc.num_fields; i++) {
                    int field_align = arm64_type_align(type->data.struc.fields[i]);
                    if (field_align > max_align) max_align = field_align;
                }
                return max_align;
            }
        case ANVIL_TYPE_FUNC:
            return 8;
        default:
            return 8;
    }
}

bool arm64_type_is_float(anvil_type_t *type)
{
    if (!type) return false;
    return type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64;
}

/* ============================================================================
 * Stack Slot Management
 * ============================================================================ */

int arm64_alloc_stack_slot(arm64_backend_t *be, anvil_value_t *val, int size)
{
    /* Grow array if needed */
    if (be->num_stack_slots >= be->stack_slots_cap) {
        size_t new_cap = be->stack_slots_cap ? be->stack_slots_cap * 2 : 32;
        arm64_stack_slot_t *new_slots = realloc(be->stack_slots,
            new_cap * sizeof(arm64_stack_slot_t));
        if (!new_slots) return -1;
        be->stack_slots = new_slots;
        be->stack_slots_cap = new_cap;
    }
    
    /* Align size to 8 bytes minimum */
    int aligned_size = (size + 7) & ~7;
    
    /* Allocate slot (stack grows down, offsets are negative from FP) */
    be->next_stack_offset += aligned_size;
    int offset = be->next_stack_offset;
    
    arm64_stack_slot_t *slot = &be->stack_slots[be->num_stack_slots++];
    slot->value = val;
    slot->offset = offset;
    slot->size = size;
    slot->is_param = false;
    slot->is_alloca = false;
    
    return offset;
}

int arm64_get_stack_slot(arm64_backend_t *be, anvil_value_t *val)
{
    for (size_t i = 0; i < be->num_stack_slots; i++) {
        if (be->stack_slots[i].value == val) {
            return be->stack_slots[i].offset;
        }
    }
    return -1;
}

int arm64_get_or_alloc_slot(arm64_backend_t *be, anvil_value_t *val)
{
    int offset = arm64_get_stack_slot(be, val);
    if (offset < 0) {
        int size = val && val->type ? arm64_type_size(val->type) : 8;
        offset = arm64_alloc_stack_slot(be, val, size);
    }
    return offset;
}

/* ============================================================================
 * Value Location Management
 * ============================================================================ */

arm64_value_loc_t *arm64_get_value_loc(arm64_backend_t *be, anvil_value_t *val)
{
    if (!val) return NULL;
    
    uint32_t id = val->id;
    if (id >= be->value_locs_cap) {
        return NULL;
    }
    
    arm64_value_loc_t *loc = &be->value_locs[id];
    if (loc->kind == ARM64_LOC_NONE) {
        return NULL;
    }
    return loc;
}

void arm64_set_value_loc(arm64_backend_t *be, anvil_value_t *val, arm64_value_loc_t *loc)
{
    if (!val) return;
    
    uint32_t id = val->id;
    
    /* Grow array if needed */
    if (id >= be->value_locs_cap) {
        size_t new_cap = be->value_locs_cap ? be->value_locs_cap * 2 : 256;
        while (new_cap <= id) new_cap *= 2;
        
        arm64_value_loc_t *new_locs = realloc(be->value_locs,
            new_cap * sizeof(arm64_value_loc_t));
        if (!new_locs) return;
        
        /* Zero-initialize new entries */
        memset(&new_locs[be->value_locs_cap], 0,
            (new_cap - be->value_locs_cap) * sizeof(arm64_value_loc_t));
        
        be->value_locs = new_locs;
        be->value_locs_cap = new_cap;
    }
    
    be->value_locs[id] = *loc;
}

/* ============================================================================
 * Code Emission Helpers
 * ============================================================================ */

void arm64_emit_mov_imm(arm64_backend_t *be, int reg, int64_t imm)
{
    const char *xreg = arm64_xreg_names[reg];
    
    if (imm >= 0 && imm <= 65535) {
        /* Small positive - single MOV */
        anvil_strbuf_appendf(&be->code, "\tmov %s, #%lld\n", xreg, (long long)imm);
    } else if (imm >= -65536 && imm < 0) {
        /* Small negative - single MOV (assembler handles it) */
        anvil_strbuf_appendf(&be->code, "\tmov %s, #%lld\n", xreg, (long long)imm);
    } else if ((uint64_t)imm <= 0xFFFFFFFF) {
        /* 32-bit value - use MOV + MOVK */
        uint32_t lo = (uint32_t)imm & 0xFFFF;
        uint32_t hi = ((uint32_t)imm >> 16) & 0xFFFF;
        anvil_strbuf_appendf(&be->code, "\tmov %s, #%u\n", xreg, lo);
        if (hi != 0) {
            anvil_strbuf_appendf(&be->code, "\tmovk %s, #%u, lsl #16\n", xreg, hi);
        }
    } else {
        /* 64-bit value - use literal pool */
        anvil_strbuf_appendf(&be->code, "\tldr %s, =%lld\n", xreg, (long long)imm);
    }
}

void arm64_emit_load_from_stack(arm64_backend_t *be, int reg, int offset, int size)
{
    const char *instr;
    const char *reg_name;
    
    switch (size) {
        case 1:
            instr = "ldrb";
            reg_name = arm64_wreg_names[reg];
            break;
        case 2:
            instr = "ldrh";
            reg_name = arm64_wreg_names[reg];
            break;
        case 4:
            instr = "ldr";
            reg_name = arm64_wreg_names[reg];
            break;
        default:
            instr = "ldr";
            reg_name = arm64_xreg_names[reg];
            break;
    }
    
    if (offset <= 255) {
        /* Small offset - direct addressing */
        anvil_strbuf_appendf(&be->code, "\t%s %s, [x29, #-%d]\n", instr, reg_name, offset);
    } else if (offset <= 4095) {
        /* Medium offset - use SUB + LDR */
        anvil_strbuf_appendf(&be->code, "\tsub x16, x29, #%d\n", offset);
        anvil_strbuf_appendf(&be->code, "\t%s %s, [x16]\n", instr, reg_name);
    } else {
        /* Large offset - use MOV + SUB + LDR */
        anvil_strbuf_appendf(&be->code, "\tmov x16, #%d\n", offset);
        anvil_strbuf_appendf(&be->code, "\tsub x16, x29, x16\n");
        anvil_strbuf_appendf(&be->code, "\t%s %s, [x16]\n", instr, reg_name);
    }
}

void arm64_emit_store_to_stack(arm64_backend_t *be, int reg, int offset, int size)
{
    const char *instr;
    const char *reg_name;
    
    if (reg == ARM64_XZR) {
        reg_name = (size <= 4) ? "wzr" : "xzr";
    } else {
        switch (size) {
            case 1:
                instr = "strb";
                reg_name = arm64_wreg_names[reg];
                break;
            case 2:
                instr = "strh";
                reg_name = arm64_wreg_names[reg];
                break;
            case 4:
                instr = "str";
                reg_name = arm64_wreg_names[reg];
                break;
            default:
                instr = "str";
                reg_name = arm64_xreg_names[reg];
                break;
        }
    }
    
    switch (size) {
        case 1: instr = "strb"; break;
        case 2: instr = "strh"; break;
        default: instr = "str"; break;
    }
    
    if (offset <= 255) {
        /* Small offset - direct addressing */
        anvil_strbuf_appendf(&be->code, "\t%s %s, [x29, #-%d]\n", instr, reg_name, offset);
    } else if (offset <= 4095) {
        /* Medium offset - use SUB + STR */
        anvil_strbuf_appendf(&be->code, "\tsub x16, x29, #%d\n", offset);
        anvil_strbuf_appendf(&be->code, "\t%s %s, [x16]\n", instr, reg_name);
    } else {
        /* Large offset - use MOV + SUB + STR */
        anvil_strbuf_appendf(&be->code, "\tmov x16, #%d\n", offset);
        anvil_strbuf_appendf(&be->code, "\tsub x16, x29, x16\n");
        anvil_strbuf_appendf(&be->code, "\t%s %s, [x16]\n", instr, reg_name);
    }
}

void arm64_emit_load_global(arm64_backend_t *be, int reg, const char *name)
{
    const char *xreg = arm64_xreg_names[reg];
    const char *prefix = arm64_symbol_prefix(be);
    
    if (arm64_is_darwin(be)) {
        anvil_strbuf_appendf(&be->code, "\tadrp %s, %s%s@PAGE\n", xreg, prefix, name);
        anvil_strbuf_appendf(&be->code, "\tadd %s, %s, %s%s@PAGEOFF\n", xreg, xreg, prefix, name);
    } else {
        anvil_strbuf_appendf(&be->code, "\tadrp %s, %s\n", xreg, name);
        anvil_strbuf_appendf(&be->code, "\tadd %s, %s, :lo12:%s\n", xreg, xreg, name);
    }
}

/* ============================================================================
 * String Table
 * ============================================================================ */

const char *arm64_add_string(arm64_backend_t *be, const char *str)
{
    /* Check if string already exists */
    for (size_t i = 0; i < be->num_strings; i++) {
        if (strcmp(be->strings[i].str, str) == 0) {
            return be->strings[i].label;
        }
    }
    
    /* Grow array if needed */
    if (be->num_strings >= be->strings_cap) {
        size_t new_cap = be->strings_cap ? be->strings_cap * 2 : 16;
        arm64_string_entry_t *new_strings = realloc(be->strings,
            new_cap * sizeof(arm64_string_entry_t));
        if (!new_strings) return ".str_err";
        be->strings = new_strings;
        be->strings_cap = new_cap;
    }
    
    /* Add new entry */
    arm64_string_entry_t *entry = &be->strings[be->num_strings];
    entry->str = str;
    entry->len = strlen(str);
    snprintf(entry->label, sizeof(entry->label), ".LC%d", be->string_counter++);
    be->num_strings++;
    
    return entry->label;
}

/* ============================================================================
 * Function Analysis
 * ============================================================================ */

void arm64_analyze_function(arm64_backend_t *be, anvil_func_t *func)
{
    if (!func) return;
    
    /* Reset frame layout */
    memset(&be->frame, 0, sizeof(be->frame));
    be->used_callee_saved = 0;
    
    /* Count allocas and instruction results */
    int num_allocas = 0;
    int num_results = 0;
    int max_call_args = 0;
    int alloca_size = 0;
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            be->total_instrs++;
            
            if (instr->op == ANVIL_OP_ALLOCA) {
                num_allocas++;
                /* Calculate alloca size */
                int size = 8;
                if (instr->result && instr->result->type &&
                    instr->result->type->kind == ANVIL_TYPE_PTR &&
                    instr->result->type->data.pointee) {
                    size = arm64_type_size(instr->result->type->data.pointee);
                }
                alloca_size += (size + 7) & ~7;  /* 8-byte aligned */
            }
            
            if (instr->result) {
                num_results++;
            }
            
            if (instr->op == ANVIL_OP_CALL) {
                int call_args = (int)instr->num_operands - 1;
                if (call_args > max_call_args) {
                    max_call_args = call_args;
                }
            }
        }
    }
    
    /* Calculate frame layout */
    
    /* Callee-saved registers: we'll determine this during code generation */
    be->frame.callee_saved_offset = 0;
    be->frame.callee_saved_size = 0;  /* Will be updated */
    
    /* Locals (allocas) */
    be->frame.locals_offset = be->frame.callee_saved_size;
    be->frame.locals_size = alloca_size;
    
    /* Spill slots for instruction results */
    be->frame.spill_offset = be->frame.locals_offset + be->frame.locals_size;
    be->frame.spill_size = num_results * 8;  /* Conservative: 8 bytes per result */
    
    /* Parameter save area (for parameters passed in registers) */
    int param_save_size = (int)func->num_params * 8;
    if (param_save_size > 64) param_save_size = 64;  /* Max 8 reg params */
    be->frame.spill_size += param_save_size;
    
    /* Outgoing arguments (for calls with >8 args) */
    be->frame.outgoing_offset = be->frame.spill_offset + be->frame.spill_size;
    be->frame.outgoing_size = (max_call_args > 8) ? (max_call_args - 8) * 8 : 0;
    
    /* Total frame size (16-byte aligned) */
    int total = be->frame.outgoing_offset + be->frame.outgoing_size;
    be->frame.total_size = (total + 15) & ~15;
    
    /* Store in function for reference */
    func->stack_size = be->frame.total_size;
    func->max_call_args = max_call_args;
}
