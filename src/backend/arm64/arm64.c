/*
 * ANVIL - ARM64 (AArch64) Backend
 * 
 * Little-endian, stack grows downward
 * Generates GAS syntax (GNU Assembler)
 * Uses AAPCS64 (ARM 64-bit Procedure Call Standard)
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ARM64 general-purpose registers (64-bit) */
static const char *arm64_xreg_names[] = {
    "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
    "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
    "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
    "x24", "x25", "x26", "x27", "x28", "x29", "x30", "sp"
};

/* ARM64 general-purpose registers (32-bit) */
static const char *arm64_wreg_names[] = {
    "w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7",
    "w8", "w9", "w10", "w11", "w12", "w13", "w14", "w15",
    "w16", "w17", "w18", "w19", "w20", "w21", "w22", "w23",
    "w24", "w25", "w26", "w27", "w28", "w29", "w30", "wsp"
};

/* AAPCS64: argument registers x0-x7, return in x0 */
#define ARM64_NUM_ARG_REGS 8

/* Register indices */
#define ARM64_X0  0
#define ARM64_X1  1
#define ARM64_X2  2
#define ARM64_X3  3
#define ARM64_X4  4
#define ARM64_X5  5
#define ARM64_X6  6
#define ARM64_X7  7
#define ARM64_X8  8   /* Indirect result location register */
#define ARM64_X9  9   /* Temporary */
#define ARM64_X10 10  /* Temporary */
#define ARM64_X11 11  /* Temporary */
#define ARM64_X12 12  /* Temporary */
#define ARM64_X13 13  /* Temporary */
#define ARM64_X14 14  /* Temporary */
#define ARM64_X15 15  /* Temporary */
#define ARM64_X16 16  /* IP0 - Intra-procedure scratch */
#define ARM64_X17 17  /* IP1 - Intra-procedure scratch */
#define ARM64_X18 18  /* Platform register (reserved) */
#define ARM64_X19 19  /* Callee-saved */
#define ARM64_X20 20  /* Callee-saved */
#define ARM64_X21 21  /* Callee-saved */
#define ARM64_X22 22  /* Callee-saved */
#define ARM64_X23 23  /* Callee-saved */
#define ARM64_X24 24  /* Callee-saved */
#define ARM64_X25 25  /* Callee-saved */
#define ARM64_X26 26  /* Callee-saved */
#define ARM64_X27 27  /* Callee-saved */
#define ARM64_X28 28  /* Callee-saved */
#define ARM64_FP  29  /* Frame pointer (x29) */
#define ARM64_LR  30  /* Link register (x30) */
#define ARM64_SP  31  /* Stack pointer */

/* String table entry */
typedef struct {
    const char *str;
    char label[32];
    size_t len;
} arm64_string_entry_t;

/* Stack slot tracking */
typedef struct {
    anvil_value_t *value;
    int offset;
} arm64_stack_slot_t;

/* Backend private data */
typedef struct {
    anvil_strbuf_t code;
    anvil_strbuf_t data;
    int label_counter;
    int string_counter;
    int stack_size;
    
    /* Stack slots for local variables */
    arm64_stack_slot_t *stack_slots;
    size_t num_stack_slots;
    size_t stack_slots_cap;
    int next_stack_offset;
    
    /* String table */
    arm64_string_entry_t *strings;
    size_t num_strings;
    size_t strings_cap;
    
    /* Current function being generated */
    anvil_func_t *current_func;
    
    /* Context reference for ABI detection */
    anvil_ctx_t *ctx;
} arm64_backend_t;

static const anvil_arch_info_t arm64_arch_info = {
    .arch = ANVIL_ARCH_ARM64,
    .name = "ARM64",
    .ptr_size = 8,
    .addr_bits = 64,
    .word_size = 8,
    .num_gpr = 31,
    .num_fpr = 32,
    .endian = ANVIL_ENDIAN_LITTLE,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false
};

static anvil_error_t arm64_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    arm64_backend_t *priv = calloc(1, sizeof(arm64_backend_t));
    if (!priv) return ANVIL_ERR_NOMEM;
    
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->ctx = ctx;
    
    be->priv = priv;
    return ANVIL_OK;
}

static void arm64_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    arm64_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv->strings);
    free(priv->stack_slots);
    free(priv);
    be->priv = NULL;
}

static const anvil_arch_info_t *arm64_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &arm64_arch_info;
}

/* Get register name based on size */
static const char *arm64_get_reg(int reg, int size)
{
    if (reg == ARM64_SP) return size <= 4 ? "wsp" : "sp";
    if (size <= 4) return arm64_wreg_names[reg];
    return arm64_xreg_names[reg];
}

/* Add stack slot for local variable */
static int arm64_add_stack_slot(arm64_backend_t *be, anvil_value_t *val)
{
    if (be->num_stack_slots >= be->stack_slots_cap) {
        size_t new_cap = be->stack_slots_cap ? be->stack_slots_cap * 2 : 16;
        arm64_stack_slot_t *new_slots = realloc(be->stack_slots,
            new_cap * sizeof(arm64_stack_slot_t));
        if (!new_slots) return -1;
        be->stack_slots = new_slots;
        be->stack_slots_cap = new_cap;
    }
    
    /* ARM64 stack grows down, allocate 8 bytes per slot */
    be->next_stack_offset += 8;
    int offset = be->next_stack_offset;
    
    be->stack_slots[be->num_stack_slots].value = val;
    be->stack_slots[be->num_stack_slots].offset = offset;
    be->num_stack_slots++;
    
    return offset;
}

/* Get stack slot offset for a value */
static int arm64_get_stack_slot(arm64_backend_t *be, anvil_value_t *val)
{
    for (size_t i = 0; i < be->num_stack_slots; i++) {
        if (be->stack_slots[i].value == val) {
            return be->stack_slots[i].offset;
        }
    }
    return -1;
}

/* Forward declarations */
static void arm64_emit_load_value(arm64_backend_t *be, anvil_value_t *val, int target_reg);

/* Add string to string table */
static const char *arm64_add_string(arm64_backend_t *be, const char *str)
{
    for (size_t i = 0; i < be->num_strings; i++) {
        if (strcmp(be->strings[i].str, str) == 0) {
            return be->strings[i].label;
        }
    }
    
    if (be->num_strings >= be->strings_cap) {
        size_t new_cap = be->strings_cap ? be->strings_cap * 2 : 16;
        arm64_string_entry_t *new_strings = realloc(be->strings,
            new_cap * sizeof(arm64_string_entry_t));
        if (!new_strings) return ".str_err";
        be->strings = new_strings;
        be->strings_cap = new_cap;
    }
    
    arm64_string_entry_t *entry = &be->strings[be->num_strings];
    entry->str = str;
    entry->len = strlen(str);
    snprintf(entry->label, sizeof(entry->label), ".LC%d", be->string_counter++);
    be->num_strings++;
    
    return entry->label;
}

/* Emit floating-point value to FP register */
static void arm64_emit_load_fp_value(arm64_backend_t *be, anvil_value_t *val, int target_dreg)
{
    if (!val) return;
    
    const char *dreg = target_dreg == 0 ? "d0" : (target_dreg == 1 ? "d1" : "d2");
    const char *sreg = target_dreg == 0 ? "s0" : (target_dreg == 1 ? "s1" : "s2");
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_FLOAT:
            /* Load FP constant - use literal pool */
            if (val->type && val->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_appendf(&be->code, "\tldr %s, =0x%08x\n", sreg,
                    *(uint32_t*)&(float){(float)val->data.f});
            } else {
                anvil_strbuf_appendf(&be->code, "\tldr %s, =0x%016llx\n", dreg,
                    *(uint64_t*)&val->data.f);
            }
            break;
            
        case ANVIL_VAL_INSTR:
            /* FP result - assume in d0 */
            if (target_dreg != 0) {
                anvil_strbuf_appendf(&be->code, "\tfmov %s, d0\n", dreg);
            }
            break;
            
        case ANVIL_VAL_PARAM:
            /* FP parameters in d0-d7 (AAPCS64) */
            {
                size_t idx = val->data.param.index;
                if (idx < 8) {
                    if (target_dreg != (int)idx) {
                        anvil_strbuf_appendf(&be->code, "\tfmov %s, d%zu\n", dreg, idx);
                    }
                } else {
                    /* Load from stack */
                    int offset = 16 + (int)(idx - 8) * 8;
                    anvil_strbuf_appendf(&be->code, "\tldr %s, [x29, #%d]\n", dreg, offset);
                }
            }
            break;
            
        default:
            /* Load as integer then move to FP */
            arm64_emit_load_value(be, val, ARM64_X9);
            anvil_strbuf_appendf(&be->code, "\tfmov %s, x9\n", dreg);
            break;
    }
}

/* Emit function prologue */
static void arm64_emit_prologue(arm64_backend_t *be, anvil_func_t *func)
{
    bool is_darwin = be->ctx && be->ctx->abi == ANVIL_ABI_DARWIN;
    
    if (is_darwin) {
        /* macOS/Darwin: underscore prefix, no .type directive */
        anvil_strbuf_appendf(&be->code, "\t.globl _%s\n", func->name);
        anvil_strbuf_appendf(&be->code, "\t.p2align 2\n");
        anvil_strbuf_appendf(&be->code, "_%s:\n", func->name);
    } else {
        /* Linux/ELF: no prefix, .type directive */
        anvil_strbuf_appendf(&be->code, "\t.globl %s\n", func->name);
        anvil_strbuf_appendf(&be->code, "\t.type %s, %%function\n", func->name);
        anvil_strbuf_appendf(&be->code, "%s:\n", func->name);
    }
    
    /* Save frame pointer and link register */
    anvil_strbuf_append(&be->code, "\tstp x29, x30, [sp, #-16]!\n");
    anvil_strbuf_append(&be->code, "\tmov x29, sp\n");
    
    /* Allocate stack space for locals (16-byte aligned) */
    size_t stack_size = (be->next_stack_offset + 15) & ~15;
    if (stack_size > 0) {
        anvil_strbuf_appendf(&be->code, "\tsub sp, sp, #%zu\n", stack_size);
    }
    be->stack_size = (int)stack_size;
}

/* Emit function epilogue */
static void arm64_emit_epilogue(arm64_backend_t *be)
{
    /* Restore stack */
    if (be->stack_size > 0) {
        anvil_strbuf_appendf(&be->code, "\tadd sp, sp, #%d\n", be->stack_size);
    }
    
    /* Restore frame pointer and link register, return */
    anvil_strbuf_append(&be->code, "\tldp x29, x30, [sp], #16\n");
    anvil_strbuf_append(&be->code, "\tret\n");
}

/* Load a value into a register */
static void arm64_emit_load_value(arm64_backend_t *be, anvil_value_t *val, int target_reg)
{
    if (!val) return;
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_INT:
            if (val->data.i >= 0 && val->data.i <= 65535) {
                /* Use MOV for small positive immediates */
                anvil_strbuf_appendf(&be->code, "\tmov %s, #%lld\n",
                    arm64_xreg_names[target_reg], (long long)val->data.i);
            } else if (val->data.i >= -65536 && val->data.i < 0) {
                /* Use MOVN for small negative immediates */
                anvil_strbuf_appendf(&be->code, "\tmov %s, #%lld\n",
                    arm64_xreg_names[target_reg], (long long)val->data.i);
            } else {
                /* Use LDR with literal pool for large constants */
                anvil_strbuf_appendf(&be->code, "\tldr %s, =%lld\n",
                    arm64_xreg_names[target_reg], (long long)val->data.i);
            }
            break;
            
        case ANVIL_VAL_CONST_NULL:
            anvil_strbuf_appendf(&be->code, "\tmov %s, #0\n",
                arm64_xreg_names[target_reg]);
            break;
            
        case ANVIL_VAL_CONST_STRING:
            {
                const char *label = arm64_add_string(be, val->data.str);
                bool is_darwin = be->ctx && be->ctx->abi == ANVIL_ABI_DARWIN;
                if (is_darwin) {
                    anvil_strbuf_appendf(&be->code, "\tadrp %s, %s@PAGE\n",
                        arm64_xreg_names[target_reg], label);
                    anvil_strbuf_appendf(&be->code, "\tadd %s, %s, %s@PAGEOFF\n",
                        arm64_xreg_names[target_reg], arm64_xreg_names[target_reg], label);
                } else {
                    anvil_strbuf_appendf(&be->code, "\tadrp %s, %s\n",
                        arm64_xreg_names[target_reg], label);
                    anvil_strbuf_appendf(&be->code, "\tadd %s, %s, :lo12:%s\n",
                        arm64_xreg_names[target_reg], arm64_xreg_names[target_reg], label);
                }
            }
            break;
            
        case ANVIL_VAL_PARAM:
            {
                size_t idx = val->data.param.index;
                if (idx < ARM64_NUM_ARG_REGS) {
                    if (target_reg != (int)idx) {
                        anvil_strbuf_appendf(&be->code, "\tmov %s, %s\n",
                            arm64_xreg_names[target_reg], arm64_xreg_names[idx]);
                    }
                } else {
                    /* Load from stack (parameters beyond x7) */
                    int offset = 16 + (int)(idx - ARM64_NUM_ARG_REGS) * 8;
                    anvil_strbuf_appendf(&be->code, "\tldr %s, [x29, #%d]\n",
                        arm64_xreg_names[target_reg], offset);
                }
            }
            break;
            
        case ANVIL_VAL_INSTR:
            if (val->data.instr && val->data.instr->op == ANVIL_OP_ALLOCA) {
                /* Load address of stack slot */
                int offset = arm64_get_stack_slot(be, val);
                if (offset >= 0) {
                    anvil_strbuf_appendf(&be->code, "\tsub %s, x29, #%d\n",
                        arm64_xreg_names[target_reg], offset);
                }
            } else {
                /* Result should be in x0 by convention */
                if (target_reg != ARM64_X0) {
                    anvil_strbuf_appendf(&be->code, "\tmov %s, x0\n",
                        arm64_xreg_names[target_reg]);
                }
            }
            break;
            
        case ANVIL_VAL_GLOBAL:
            /* Load address of global using ADRP + ADD */
            {
                bool is_darwin = be->ctx && be->ctx->abi == ANVIL_ABI_DARWIN;
                const char *prefix = is_darwin ? "_" : "";
                if (is_darwin) {
                    anvil_strbuf_appendf(&be->code, "\tadrp %s, %s%s@PAGE\n",
                        arm64_xreg_names[target_reg], prefix, val->name);
                    anvil_strbuf_appendf(&be->code, "\tadd %s, %s, %s%s@PAGEOFF\n",
                        arm64_xreg_names[target_reg], arm64_xreg_names[target_reg], prefix, val->name);
                } else {
                    anvil_strbuf_appendf(&be->code, "\tadrp %s, %s\n",
                        arm64_xreg_names[target_reg], val->name);
                    anvil_strbuf_appendf(&be->code, "\tadd %s, %s, :lo12:%s\n",
                        arm64_xreg_names[target_reg], arm64_xreg_names[target_reg], val->name);
                }
            }
            break;
            
        case ANVIL_VAL_FUNC:
            /* Load function address */
            {
                bool is_darwin = be->ctx && be->ctx->abi == ANVIL_ABI_DARWIN;
                const char *prefix = is_darwin ? "_" : "";
                if (is_darwin) {
                    anvil_strbuf_appendf(&be->code, "\tadrp %s, %s%s@PAGE\n",
                        arm64_xreg_names[target_reg], prefix, val->name);
                    anvil_strbuf_appendf(&be->code, "\tadd %s, %s, %s%s@PAGEOFF\n",
                        arm64_xreg_names[target_reg], arm64_xreg_names[target_reg], prefix, val->name);
                } else {
                    anvil_strbuf_appendf(&be->code, "\tadrp %s, %s\n",
                        arm64_xreg_names[target_reg], val->name);
                    anvil_strbuf_appendf(&be->code, "\tadd %s, %s, :lo12:%s\n",
                        arm64_xreg_names[target_reg], arm64_xreg_names[target_reg], val->name);
                }
            }
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "\t// Unknown value kind %d\n", val->kind);
            break;
    }
}

/* Emit a single instruction */
static void arm64_emit_instr(arm64_backend_t *be, anvil_instr_t *instr)
{
    if (!instr) return;
    
    switch (instr->op) {
        case ANVIL_OP_PHI:
            /* PHI nodes handled during SSA resolution */
            break;
            
        case ANVIL_OP_ALLOCA:
            {
                int offset = arm64_add_stack_slot(be, instr->result);
                /* Zero-initialize the slot */
                anvil_strbuf_appendf(&be->code, "\tstr xzr, [x29, #-%d]\n", offset);
            }
            break;
            
        case ANVIL_OP_ADD:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tadd x0, x9, x10\n");
            break;
            
        case ANVIL_OP_SUB:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tsub x0, x9, x10\n");
            break;
            
        case ANVIL_OP_MUL:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tmul x0, x9, x10\n");
            break;
            
        case ANVIL_OP_SDIV:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tsdiv x0, x9, x10\n");
            break;
            
        case ANVIL_OP_UDIV:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tudiv x0, x9, x10\n");
            break;
            
        case ANVIL_OP_SMOD:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tsdiv x11, x9, x10\n");
            anvil_strbuf_append(&be->code, "\tmsub x0, x11, x10, x9\n");
            break;
            
        case ANVIL_OP_UMOD:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tudiv x11, x9, x10\n");
            anvil_strbuf_append(&be->code, "\tmsub x0, x11, x10, x9\n");
            break;
            
        case ANVIL_OP_AND:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tand x0, x9, x10\n");
            break;
            
        case ANVIL_OP_OR:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\torr x0, x9, x10\n");
            break;
            
        case ANVIL_OP_XOR:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\teor x0, x9, x10\n");
            break;
            
        case ANVIL_OP_NOT:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            anvil_strbuf_append(&be->code, "\tmvn x0, x9\n");
            break;
            
        case ANVIL_OP_SHL:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tlsl x0, x9, x10\n");
            break;
            
        case ANVIL_OP_SHR:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tlsr x0, x9, x10\n");
            break;
            
        case ANVIL_OP_SAR:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tasr x0, x9, x10\n");
            break;
            
        case ANVIL_OP_NEG:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            anvil_strbuf_append(&be->code, "\tneg x0, x9\n");
            break;
            
        case ANVIL_OP_LOAD:
            /* Check if loading from stack slot */
            if (instr->operands[0]->kind == ANVIL_VAL_INSTR &&
                instr->operands[0]->data.instr &&
                instr->operands[0]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = arm64_get_stack_slot(be, instr->operands[0]);
                if (offset >= 0) {
                    anvil_strbuf_appendf(&be->code, "\tldr x0, [x29, #-%d]\n", offset);
                    break;
                }
            }
            /* Check if loading from global */
            if (instr->operands[0]->kind == ANVIL_VAL_GLOBAL) {
                anvil_strbuf_appendf(&be->code, "\tadrp x9, %s\n", instr->operands[0]->name);
                anvil_strbuf_appendf(&be->code, "\tldr x0, [x9, :lo12:%s]\n", instr->operands[0]->name);
                break;
            }
            /* Generic load */
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            anvil_strbuf_append(&be->code, "\tldr x0, [x9]\n");
            break;
            
        case ANVIL_OP_STORE:
            /* Check if storing to stack slot */
            if (instr->operands[1]->kind == ANVIL_VAL_INSTR &&
                instr->operands[1]->data.instr &&
                instr->operands[1]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = arm64_get_stack_slot(be, instr->operands[1]);
                if (offset >= 0) {
                    arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
                    anvil_strbuf_appendf(&be->code, "\tstr x9, [x29, #-%d]\n", offset);
                    break;
                }
            }
            /* Check if storing to global */
            if (instr->operands[1]->kind == ANVIL_VAL_GLOBAL) {
                arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
                anvil_strbuf_appendf(&be->code, "\tadrp x10, %s\n", instr->operands[1]->name);
                anvil_strbuf_appendf(&be->code, "\tstr x9, [x10, :lo12:%s]\n", instr->operands[1]->name);
                break;
            }
            /* Generic store */
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tstr x9, [x10]\n");
            break;
            
        case ANVIL_OP_GEP:
            /* Get Element Pointer - array indexing */
            {
                arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
                
                if (instr->num_operands > 1) {
                    arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
                    
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
                        anvil_strbuf_append(&be->code, "\tadd x0, x9, x10\n");
                    } else if (elem_size == 2) {
                        anvil_strbuf_append(&be->code, "\tadd x0, x9, x10, lsl #1\n");
                    } else if (elem_size == 4) {
                        anvil_strbuf_append(&be->code, "\tadd x0, x9, x10, lsl #2\n");
                    } else {
                        anvil_strbuf_append(&be->code, "\tadd x0, x9, x10, lsl #3\n");
                    }
                } else {
                    anvil_strbuf_append(&be->code, "\tmov x0, x9\n");
                }
            }
            break;
            
        case ANVIL_OP_STRUCT_GEP:
            /* Get Struct Field Pointer */
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
                } else {
                    anvil_strbuf_appendf(&be->code, "\tadd x0, x9, #%d\n", offset);
                }
            }
            break;
            
        case ANVIL_OP_CMP_EQ:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tcmp x9, x10\n");
            anvil_strbuf_append(&be->code, "\tcset x0, eq\n");
            break;
            
        case ANVIL_OP_CMP_NE:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tcmp x9, x10\n");
            anvil_strbuf_append(&be->code, "\tcset x0, ne\n");
            break;
            
        case ANVIL_OP_CMP_LT:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tcmp x9, x10\n");
            anvil_strbuf_append(&be->code, "\tcset x0, lt\n");
            break;
            
        case ANVIL_OP_CMP_LE:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tcmp x9, x10\n");
            anvil_strbuf_append(&be->code, "\tcset x0, le\n");
            break;
            
        case ANVIL_OP_CMP_GT:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tcmp x9, x10\n");
            anvil_strbuf_append(&be->code, "\tcset x0, gt\n");
            break;
            
        case ANVIL_OP_CMP_GE:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tcmp x9, x10\n");
            anvil_strbuf_append(&be->code, "\tcset x0, ge\n");
            break;
            
        case ANVIL_OP_CMP_ULT:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tcmp x9, x10\n");
            anvil_strbuf_append(&be->code, "\tcset x0, lo\n");
            break;
            
        case ANVIL_OP_CMP_ULE:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tcmp x9, x10\n");
            anvil_strbuf_append(&be->code, "\tcset x0, ls\n");
            break;
            
        case ANVIL_OP_CMP_UGT:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tcmp x9, x10\n");
            anvil_strbuf_append(&be->code, "\tcset x0, hi\n");
            break;
            
        case ANVIL_OP_CMP_UGE:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10);
            anvil_strbuf_append(&be->code, "\tcmp x9, x10\n");
            anvil_strbuf_append(&be->code, "\tcset x0, hs\n");
            break;
            
        case ANVIL_OP_BR:
            /* Unconditional branch - use true_block */
            if (instr->true_block) {
                anvil_strbuf_appendf(&be->code, "\tb .L%s_%s\n",
                    be->current_func->name, instr->true_block->name);
            }
            break;
            
        case ANVIL_OP_BR_COND:
            /* Conditional branch - use true_block and false_block */
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            if (instr->true_block && instr->false_block) {
                anvil_strbuf_appendf(&be->code, "\tcbnz x9, .L%s_%s\n",
                    be->current_func->name, instr->true_block->name);
                anvil_strbuf_appendf(&be->code, "\tb .L%s_%s\n",
                    be->current_func->name, instr->false_block->name);
            }
            break;
            
        case ANVIL_OP_CALL:
            {
                /* Load arguments into x0-x7 */
                for (size_t i = 1; i < instr->num_operands && i <= ARM64_NUM_ARG_REGS; i++) {
                    arm64_emit_load_value(be, instr->operands[i], (int)(i - 1));
                }
                
                /* Call function */
                if (instr->operands[0]->kind == ANVIL_VAL_FUNC) {
                    bool is_darwin = be->ctx && be->ctx->abi == ANVIL_ABI_DARWIN;
                    const char *prefix = is_darwin ? "_" : "";
                    anvil_strbuf_appendf(&be->code, "\tbl %s%s\n", prefix, instr->operands[0]->name);
                } else {
                    arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
                    anvil_strbuf_append(&be->code, "\tblr x9\n");
                }
            }
            break;
            
        case ANVIL_OP_RET:
            if (instr->num_operands > 0 && instr->operands[0]) {
                arm64_emit_load_value(be, instr->operands[0], ARM64_X0);
            }
            arm64_emit_epilogue(be);
            break;
            
        case ANVIL_OP_TRUNC:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X0);
            /* Truncation is implicit in ARM64 when using w registers */
            break;
            
        case ANVIL_OP_ZEXT:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            /* Zero extension - use UXTB, UXTH, or UXTW */
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
            break;
            
        case ANVIL_OP_SEXT:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            /* Sign extension - use SXTB, SXTH, or SXTW */
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
            break;
            
        case ANVIL_OP_BITCAST:
        case ANVIL_OP_PTRTOINT:
        case ANVIL_OP_INTTOPTR:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X0);
            break;
            
        case ANVIL_OP_SELECT:
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);  /* condition */
            arm64_emit_load_value(be, instr->operands[1], ARM64_X10); /* true value */
            arm64_emit_load_value(be, instr->operands[2], ARM64_X11); /* false value */
            anvil_strbuf_append(&be->code, "\tcmp x9, #0\n");
            anvil_strbuf_append(&be->code, "\tcsel x0, x10, x11, ne\n");
            break;
            
        /* Floating-point operations (IEEE 754) */
        case ANVIL_OP_FADD:
            arm64_emit_load_fp_value(be, instr->operands[0], 0);
            arm64_emit_load_fp_value(be, instr->operands[1], 1);
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfadd s0, s0, s1\n");
            } else {
                anvil_strbuf_append(&be->code, "\tfadd d0, d0, d1\n");
            }
            break;
            
        case ANVIL_OP_FSUB:
            arm64_emit_load_fp_value(be, instr->operands[0], 0);
            arm64_emit_load_fp_value(be, instr->operands[1], 1);
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfsub s0, s0, s1\n");
            } else {
                anvil_strbuf_append(&be->code, "\tfsub d0, d0, d1\n");
            }
            break;
            
        case ANVIL_OP_FMUL:
            arm64_emit_load_fp_value(be, instr->operands[0], 0);
            arm64_emit_load_fp_value(be, instr->operands[1], 1);
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfmul s0, s0, s1\n");
            } else {
                anvil_strbuf_append(&be->code, "\tfmul d0, d0, d1\n");
            }
            break;
            
        case ANVIL_OP_FDIV:
            arm64_emit_load_fp_value(be, instr->operands[0], 0);
            arm64_emit_load_fp_value(be, instr->operands[1], 1);
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfdiv s0, s0, s1\n");
            } else {
                anvil_strbuf_append(&be->code, "\tfdiv d0, d0, d1\n");
            }
            break;
            
        case ANVIL_OP_FNEG:
            arm64_emit_load_fp_value(be, instr->operands[0], 0);
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfneg s0, s0\n");
            } else {
                anvil_strbuf_append(&be->code, "\tfneg d0, d0\n");
            }
            break;
            
        case ANVIL_OP_FABS:
            arm64_emit_load_fp_value(be, instr->operands[0], 0);
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfabs s0, s0\n");
            } else {
                anvil_strbuf_append(&be->code, "\tfabs d0, d0\n");
            }
            break;
            
        case ANVIL_OP_FCMP:
            /* Floating-point compare */
            arm64_emit_load_fp_value(be, instr->operands[0], 0);
            arm64_emit_load_fp_value(be, instr->operands[1], 1);
            if (instr->operands[0]->type &&
                instr->operands[0]->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfcmp s0, s1\n");
            } else {
                anvil_strbuf_append(&be->code, "\tfcmp d0, d1\n");
            }
            anvil_strbuf_append(&be->code, "\tcset x0, eq\n");
            break;
            
        case ANVIL_OP_SITOFP:
            /* Signed integer to floating-point */
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tscvtf s0, x9\n");
                anvil_strbuf_append(&be->code, "\tfmov w0, s0\n");
            } else {
                anvil_strbuf_append(&be->code, "\tscvtf d0, x9\n");
                anvil_strbuf_append(&be->code, "\tfmov x0, d0\n");
            }
            break;
            
        case ANVIL_OP_UITOFP:
            /* Unsigned integer to floating-point */
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tucvtf s0, x9\n");
                anvil_strbuf_append(&be->code, "\tfmov w0, s0\n");
            } else {
                anvil_strbuf_append(&be->code, "\tucvtf d0, x9\n");
                anvil_strbuf_append(&be->code, "\tfmov x0, d0\n");
            }
            break;
            
        case ANVIL_OP_FPTOSI:
            /* Floating-point to signed integer */
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            if (instr->operands[0]->type &&
                instr->operands[0]->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfmov s0, w9\n");
                anvil_strbuf_append(&be->code, "\tfcvtzs x0, s0\n");
            } else {
                anvil_strbuf_append(&be->code, "\tfmov d0, x9\n");
                anvil_strbuf_append(&be->code, "\tfcvtzs x0, d0\n");
            }
            break;
            
        case ANVIL_OP_FPTOUI:
            /* Floating-point to unsigned integer */
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            if (instr->operands[0]->type &&
                instr->operands[0]->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfmov s0, w9\n");
                anvil_strbuf_append(&be->code, "\tfcvtzu x0, s0\n");
            } else {
                anvil_strbuf_append(&be->code, "\tfmov d0, x9\n");
                anvil_strbuf_append(&be->code, "\tfcvtzu x0, d0\n");
            }
            break;
            
        case ANVIL_OP_FPEXT:
            /* Extend float to double */
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            anvil_strbuf_append(&be->code, "\tfmov s0, w9\n");
            anvil_strbuf_append(&be->code, "\tfcvt d0, s0\n");
            anvil_strbuf_append(&be->code, "\tfmov x0, d0\n");
            break;
            
        case ANVIL_OP_FPTRUNC:
            /* Truncate double to float */
            arm64_emit_load_value(be, instr->operands[0], ARM64_X9);
            anvil_strbuf_append(&be->code, "\tfmov d0, x9\n");
            anvil_strbuf_append(&be->code, "\tfcvt s0, d0\n");
            anvil_strbuf_append(&be->code, "\tfmov w0, s0\n");
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "\t// Unimplemented op %d\n", instr->op);
            break;
    }
}

/* Emit a basic block */
static void arm64_emit_block(arm64_backend_t *be, anvil_block_t *block)
{
    if (!block) return;
    
    /* Emit label (skip for entry block) */
    if (block != be->current_func->blocks) {
        anvil_strbuf_appendf(&be->code, ".L%s_%s:\n",
            be->current_func->name, block->name);
    }
    
    /* Emit instructions */
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        arm64_emit_instr(be, instr);
    }
}

/* Emit a function */
static void arm64_emit_func(arm64_backend_t *be, anvil_func_t *func)
{
    if (!func || func->is_declaration) return;
    
    be->current_func = func;
    be->num_stack_slots = 0;
    be->next_stack_offset = 0;
    
    /* First pass: count stack slots needed */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_ALLOCA) {
                arm64_add_stack_slot(be, instr->result);
            }
        }
    }
    
    /* Emit prologue */
    arm64_emit_prologue(be, func);
    
    /* Emit blocks */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        arm64_emit_block(be, block);
    }
    
    /* .size directive only for ELF (Linux), not for Mach-O (macOS) */
    bool is_darwin = be->ctx && be->ctx->abi == ANVIL_ABI_DARWIN;
    if (!is_darwin) {
        anvil_strbuf_appendf(&be->code, "\t.size %s, .-%s\n", func->name, func->name);
    }
    anvil_strbuf_append(&be->code, "\n");
}

/* Emit global variables */
static void arm64_emit_globals(arm64_backend_t *be, anvil_module_t *mod)
{
    if (mod->num_globals == 0) return;
    
    bool is_darwin = be->ctx && be->ctx->abi == ANVIL_ABI_DARWIN;
    const char *prefix = is_darwin ? "_" : "";
    
    anvil_strbuf_append(&be->data, "\t.data\n");
    
    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        anvil_strbuf_appendf(&be->data, "\t.globl %s%s\n", prefix, g->value->name);
        
        /* Determine alignment and size based on type */
        int size = 8;
        int align = 8;
        anvil_type_t *type = g->value->type;
        
        if (type) {
            switch (type->kind) {
                case ANVIL_TYPE_I8:
                case ANVIL_TYPE_U8:
                    size = 1;
                    align = 1;
                    break;
                case ANVIL_TYPE_I16:
                case ANVIL_TYPE_U16:
                    size = 2;
                    align = 2;
                    break;
                case ANVIL_TYPE_I32:
                case ANVIL_TYPE_U32:
                case ANVIL_TYPE_F32:
                    size = 4;
                    align = 4;
                    break;
                default:
                    size = 8;
                    align = 8;
                    break;
            }
        }
        
        if (is_darwin) {
            anvil_strbuf_appendf(&be->data, "\t.p2align %d\n", align == 1 ? 0 : align == 2 ? 1 : align == 4 ? 2 : 3);
        } else {
            anvil_strbuf_appendf(&be->data, "\t.align %d\n", align);
        }
        anvil_strbuf_appendf(&be->data, "%s%s:\n", prefix, g->value->name);
        
        /* Check for initializer */
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

/* Emit string constants */
static void arm64_emit_strings(arm64_backend_t *be)
{
    if (be->num_strings == 0) return;
    
    bool is_darwin = be->ctx && be->ctx->abi == ANVIL_ABI_DARWIN;
    
    if (is_darwin) {
        anvil_strbuf_append(&be->data, "\t.section __TEXT,__cstring,cstring_literals\n");
    } else {
        anvil_strbuf_append(&be->data, "\t.section .rodata\n");
    }
    
    for (size_t i = 0; i < be->num_strings; i++) {
        arm64_string_entry_t *entry = &be->strings[i];
        anvil_strbuf_appendf(&be->data, "%s:\n", entry->label);
        anvil_strbuf_appendf(&be->data, "\t.asciz \"%s\"\n", entry->str);
    }
    
    anvil_strbuf_append(&be->data, "\n");
}

static anvil_error_t arm64_codegen_module(anvil_backend_t *be, anvil_module_t *mod,
                                          char **output, size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;
    
    arm64_backend_t *priv = be->priv;
    
    /* Reset state */
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->label_counter = 0;
    priv->num_strings = 0;
    
    /* Emit header */
    bool is_darwin = priv->ctx && priv->ctx->abi == ANVIL_ABI_DARWIN;
    
    if (is_darwin) {
        anvil_strbuf_append(&priv->code, "// Generated by ANVIL for ARM64 (AArch64) - macOS\n");
        anvil_strbuf_append(&priv->code, "\t.build_version macos, 11, 0\n");
        anvil_strbuf_append(&priv->code, "\t.section __TEXT,__text,regular,pure_instructions\n\n");
    } else {
        anvil_strbuf_append(&priv->code, "// Generated by ANVIL for ARM64 (AArch64) - Linux\n");
        anvil_strbuf_append(&priv->code, "\t.arch armv8-a\n");
        anvil_strbuf_append(&priv->code, "\t.text\n\n");
    }
    
    /* Emit functions */
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        arm64_emit_func(priv, func);
    }
    
    /* Emit globals */
    arm64_emit_globals(priv, mod);
    
    /* Emit strings */
    arm64_emit_strings(priv);
    
    /* Combine code and data sections */
    anvil_strbuf_t result;
    anvil_strbuf_init(&result);
    char *code_str = anvil_strbuf_detach(&priv->code, NULL);
    char *data_str = anvil_strbuf_detach(&priv->data, NULL);
    if (code_str) {
        anvil_strbuf_append(&result, code_str);
        free(code_str);
    }
    if (data_str) {
        anvil_strbuf_append(&result, data_str);
        free(data_str);
    }
    
    *output = anvil_strbuf_detach(&result, len);
    return ANVIL_OK;
}

static anvil_error_t arm64_codegen_func(anvil_backend_t *be, anvil_func_t *func,
                                        char **output, size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    
    arm64_backend_t *priv = be->priv;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);
    
    arm64_emit_func(priv, func);
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

const anvil_backend_ops_t anvil_backend_arm64 = {
    .name = "ARM64",
    .arch = ANVIL_ARCH_ARM64,
    .init = arm64_init,
    .cleanup = arm64_cleanup,
    .codegen_module = arm64_codegen_module,
    .codegen_func = arm64_codegen_func,
    .get_arch_info = arm64_get_arch_info
};
