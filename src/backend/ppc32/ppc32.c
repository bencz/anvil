/*
 * ANVIL - PowerPC 32-bit Backend
 * 
 * Big-endian, stack grows downward
 * Generates GAS syntax for PowerPC
 * 
 * Register conventions (System V ABI for PPC32):
 * - r0: Volatile, used in prologue/epilogue
 * - r1: Stack pointer (SP)
 * - r2: Reserved (TOC pointer in some ABIs)
 * - r3-r10: Function arguments and return values
 * - r3: Return value
 * - r11-r12: Volatile, used for linkage
 * - r13: Small data area pointer (reserved)
 * - r14-r30: Non-volatile (callee-saved)
 * - r31: Non-volatile, often used as frame pointer
 * - f0: Volatile
 * - f1-f8: Floating-point arguments
 * - f1: Floating-point return value
 * - f9-f13: Volatile
 * - f14-f31: Non-volatile (callee-saved)
 * - CR0-CR7: Condition registers
 * - LR: Link register (return address)
 * - CTR: Count register
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* PowerPC 32-bit register names */
static const char *ppc32_gpr_names[] = {
    "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
    "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23",
    "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31"
};

/* Register indices */
#define PPC_R0   0
#define PPC_R1   1   /* Stack pointer */
#define PPC_R2   2   /* TOC pointer (reserved) */
#define PPC_R3   3   /* First arg / return value */
#define PPC_R4   4
#define PPC_R5   5
#define PPC_R6   6
#define PPC_R7   7
#define PPC_R8   8
#define PPC_R9   9
#define PPC_R10  10
#define PPC_R11  11  /* Volatile, linkage */
#define PPC_R12  12  /* Volatile, linkage */
#define PPC_R31  31  /* Frame pointer */

/* Argument registers */
static const int ppc32_arg_regs[] = { PPC_R3, PPC_R4, PPC_R5, PPC_R6, PPC_R7, PPC_R8, PPC_R9, PPC_R10 };
#define PPC32_NUM_ARG_REGS 8

/* PPC32 ABI constants */
#define PPC32_MIN_FRAME_SIZE 32
#define PPC32_LR_SAVE_OFFSET 4

/* String table entry */
typedef struct {
    const char *str;
    char label[32];
    size_t len;
} ppc32_string_entry_t;

/* Stack slot tracking */
typedef struct {
    anvil_value_t *value;
    int offset;
} ppc32_stack_slot_t;

/* Backend private data */
typedef struct {
    anvil_strbuf_t code;
    anvil_strbuf_t data;
    int label_counter;
    int string_counter;
    int stack_offset;
    int local_offset;
    size_t frame_size;
    
    /* Stack slots for local variables */
    ppc32_stack_slot_t *stack_slots;
    size_t num_stack_slots;
    size_t stack_slots_cap;
    int next_stack_offset;
    
    /* String table */
    ppc32_string_entry_t *strings;
    size_t num_strings;
    size_t strings_cap;
    
    /* Current function being generated */
    anvil_func_t *current_func;
    
    /* Context reference */
    anvil_ctx_t *ctx;
} ppc32_backend_t;

static const anvil_arch_info_t ppc32_arch_info = {
    .arch = ANVIL_ARCH_PPC32,
    .name = "PowerPC 32-bit",
    .ptr_size = 4,
    .addr_bits = 32,
    .word_size = 4,
    .num_gpr = 32,
    .num_fpr = 32,
    .endian = ANVIL_ENDIAN_BIG,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false
};

/* Forward declarations */
static void ppc32_emit_func(ppc32_backend_t *be, anvil_func_t *func);
static void ppc32_emit_instr(ppc32_backend_t *be, anvil_instr_t *instr, anvil_func_t *func);

static anvil_error_t ppc32_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    ppc32_backend_t *priv = calloc(1, sizeof(ppc32_backend_t));
    if (!priv) return ANVIL_ERR_NOMEM;
    
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->label_counter = 0;
    priv->ctx = ctx;
    
    be->priv = priv;
    return ANVIL_OK;
}

static void ppc32_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    ppc32_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv->strings);
    free(priv->stack_slots);
    free(priv);
    be->priv = NULL;
}

/* Add stack slot for local variable */
static int ppc32_add_stack_slot(ppc32_backend_t *be, anvil_value_t *val)
{
    if (be->num_stack_slots >= be->stack_slots_cap) {
        size_t new_cap = be->stack_slots_cap ? be->stack_slots_cap * 2 : 16;
        ppc32_stack_slot_t *new_slots = realloc(be->stack_slots,
            new_cap * sizeof(ppc32_stack_slot_t));
        if (!new_slots) return -1;
        be->stack_slots = new_slots;
        be->stack_slots_cap = new_cap;
    }
    
    be->next_stack_offset += 4;  /* 4 bytes for 32-bit */
    int offset = be->next_stack_offset;
    
    be->stack_slots[be->num_stack_slots].value = val;
    be->stack_slots[be->num_stack_slots].offset = offset;
    be->num_stack_slots++;
    
    return offset;
}

/* Get stack slot offset for a value */
static int ppc32_get_stack_slot(ppc32_backend_t *be, anvil_value_t *val)
{
    for (size_t i = 0; i < be->num_stack_slots; i++) {
        if (be->stack_slots[i].value == val) {
            return be->stack_slots[i].offset;
        }
    }
    return -1;
}

/* Add string to string table */
static const char *ppc32_add_string(ppc32_backend_t *be, const char *str)
{
    for (size_t i = 0; i < be->num_strings; i++) {
        if (strcmp(be->strings[i].str, str) == 0) {
            return be->strings[i].label;
        }
    }
    
    if (be->num_strings >= be->strings_cap) {
        size_t new_cap = be->strings_cap ? be->strings_cap * 2 : 16;
        ppc32_string_entry_t *new_strings = realloc(be->strings,
            new_cap * sizeof(ppc32_string_entry_t));
        if (!new_strings) return ".str_err";
        be->strings = new_strings;
        be->strings_cap = new_cap;
    }
    
    ppc32_string_entry_t *entry = &be->strings[be->num_strings];
    entry->str = str;
    entry->len = strlen(str);
    snprintf(entry->label, sizeof(entry->label), ".LC%d", be->string_counter++);
    be->num_strings++;
    
    return entry->label;
}

static const anvil_arch_info_t *ppc32_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &ppc32_arch_info;
}

static void ppc32_emit_prologue(ppc32_backend_t *be, anvil_func_t *func)
{
    size_t frame_size = func->stack_size;
    if (frame_size < 32) frame_size = 32; /* Minimum frame size */
    frame_size = (frame_size + 15) & ~15; /* Align to 16 bytes */
    
    anvil_strbuf_appendf(&be->code, "\t.globl %s\n", func->name);
    anvil_strbuf_appendf(&be->code, "\t.type %s, @function\n", func->name);
    anvil_strbuf_appendf(&be->code, "%s:\n", func->name);
    
    /* Save link register */
    anvil_strbuf_append(&be->code, "\tmflr r0\n");
    anvil_strbuf_append(&be->code, "\tstw r0, 4(r1)\n");
    
    /* Save callee-saved registers if needed */
    anvil_strbuf_append(&be->code, "\tstw r31, -4(r1)\n");
    
    /* Create stack frame */
    anvil_strbuf_appendf(&be->code, "\tstwu r1, -%zu(r1)\n", frame_size);
    
    /* Set up frame pointer */
    anvil_strbuf_appendf(&be->code, "\taddi r31, r1, %zu\n", frame_size);
    
    be->local_offset = 8; /* Start of local variables area */
}

static void ppc32_emit_epilogue(ppc32_backend_t *be, anvil_func_t *func)
{
    size_t frame_size = func->stack_size;
    if (frame_size < 32) frame_size = 32;
    frame_size = (frame_size + 15) & ~15;
    
    /* Restore stack pointer */
    anvil_strbuf_appendf(&be->code, "\taddi r1, r1, %zu\n", frame_size);
    
    /* Restore callee-saved registers */
    anvil_strbuf_append(&be->code, "\tlwz r31, -4(r1)\n");
    
    /* Restore link register and return */
    anvil_strbuf_append(&be->code, "\tlwz r0, 4(r1)\n");
    anvil_strbuf_append(&be->code, "\tmtlr r0\n");
    anvil_strbuf_append(&be->code, "\tblr\n");
}

static void ppc32_emit_load_value(ppc32_backend_t *be, anvil_value_t *val, int reg, anvil_func_t *func)
{
    if (!val) return;
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_INT:
            if (val->data.i >= -32768 && val->data.i <= 32767) {
                anvil_strbuf_appendf(&be->code, "\tli %s, %lld\n",
                    ppc32_gpr_names[reg], (long long)val->data.i);
            } else {
                /* Load 32-bit immediate in two parts */
                anvil_strbuf_appendf(&be->code, "\tlis %s, %lld@ha\n",
                    ppc32_gpr_names[reg], (long long)(val->data.i >> 16) & 0xFFFF);
                anvil_strbuf_appendf(&be->code, "\tori %s, %s, %lld@l\n",
                    ppc32_gpr_names[reg], ppc32_gpr_names[reg], 
                    (long long)val->data.i & 0xFFFF);
            }
            break;
            
        case ANVIL_VAL_PARAM:
            {
                size_t idx = val->data.param.index;
                if (idx < PPC32_NUM_ARG_REGS) {
                    /* Parameter is in register */
                    if (ppc32_arg_regs[idx] != reg) {
                        anvil_strbuf_appendf(&be->code, "\tmr %s, %s\n",
                            ppc32_gpr_names[reg], ppc32_gpr_names[ppc32_arg_regs[idx]]);
                    }
                } else {
                    /* Parameter is on stack */
                    size_t offset = 8 + (idx - PPC32_NUM_ARG_REGS) * 4;
                    anvil_strbuf_appendf(&be->code, "\tlwz %s, %zu(r31)\n",
                        ppc32_gpr_names[reg], offset);
                }
            }
            break;
            
        case ANVIL_VAL_CONST_NULL:
            anvil_strbuf_appendf(&be->code, "\tli %s, 0\n", ppc32_gpr_names[reg]);
            break;
            
        case ANVIL_VAL_CONST_STRING:
            {
                const char *label = ppc32_add_string(be, val->data.str ? val->data.str : "");
                anvil_strbuf_appendf(&be->code, "\tlis %s, %s@ha\n",
                    ppc32_gpr_names[reg], label);
                anvil_strbuf_appendf(&be->code, "\taddi %s, %s, %s@l\n",
                    ppc32_gpr_names[reg], ppc32_gpr_names[reg], label);
            }
            break;
            
        case ANVIL_VAL_INSTR:
            if (val->data.instr && val->data.instr->op == ANVIL_OP_ALLOCA) {
                /* Load address of stack slot */
                int offset = ppc32_get_stack_slot(be, val);
                if (offset >= 0) {
                    anvil_strbuf_appendf(&be->code, "\taddi %s, r31, -%d\n",
                        ppc32_gpr_names[reg], PPC32_MIN_FRAME_SIZE + offset);
                }
            } else {
                /* Result should be in r3 by convention */
                if (reg != PPC_R3) {
                    anvil_strbuf_appendf(&be->code, "\tmr %s, r3\n", ppc32_gpr_names[reg]);
                }
            }
            break;
            
        case ANVIL_VAL_FUNC:
            /* Load function address */
            anvil_strbuf_appendf(&be->code, "\tlis %s, %s@ha\n",
                ppc32_gpr_names[reg], val->name);
            anvil_strbuf_appendf(&be->code, "\taddi %s, %s, %s@l\n",
                ppc32_gpr_names[reg], ppc32_gpr_names[reg], val->name);
            break;
            
        case ANVIL_VAL_GLOBAL:
            anvil_strbuf_appendf(&be->code, "\tlis %s, %s@ha\n",
                ppc32_gpr_names[reg], val->name);
            anvil_strbuf_appendf(&be->code, "\tlwz %s, %s@l(%s)\n",
                ppc32_gpr_names[reg], val->name, ppc32_gpr_names[reg]);
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "\t# unhandled value kind %d\n", val->kind);
            break;
    }
    
    (void)func;
}

static void ppc32_emit_instr(ppc32_backend_t *be, anvil_instr_t *instr, anvil_func_t *func)
{
    if (!instr) return;
    
    switch (instr->op) {
        case ANVIL_OP_ADD:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
            anvil_strbuf_append(&be->code, "\tadd r3, r3, r4\n");
            break;
            
        case ANVIL_OP_SUB:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
            anvil_strbuf_append(&be->code, "\tsub r3, r3, r4\n");
            break;
            
        case ANVIL_OP_MUL:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
            anvil_strbuf_append(&be->code, "\tmullw r3, r3, r4\n");
            break;
            
        case ANVIL_OP_SDIV:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
            anvil_strbuf_append(&be->code, "\tdivw r3, r3, r4\n");
            break;
            
        case ANVIL_OP_UDIV:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
            anvil_strbuf_append(&be->code, "\tdivwu r3, r3, r4\n");
            break;
            
        case ANVIL_OP_SMOD:
        case ANVIL_OP_UMOD:
            /* PPC doesn't have modulo - compute as: a % b = a - (a / b) * b */
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
            if (instr->op == ANVIL_OP_SMOD) {
                anvil_strbuf_append(&be->code, "\tdivw r5, r3, r4\n");
            } else {
                anvil_strbuf_append(&be->code, "\tdivwu r5, r3, r4\n");
            }
            anvil_strbuf_append(&be->code, "\tmullw r5, r5, r4\n");
            anvil_strbuf_append(&be->code, "\tsub r3, r3, r5\n");
            break;
            
        case ANVIL_OP_NEG:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            anvil_strbuf_append(&be->code, "\tneg r3, r3\n");
            break;
            
        case ANVIL_OP_AND:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
            anvil_strbuf_append(&be->code, "\tand r3, r3, r4\n");
            break;
            
        case ANVIL_OP_OR:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
            anvil_strbuf_append(&be->code, "\tor r3, r3, r4\n");
            break;
            
        case ANVIL_OP_XOR:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
            anvil_strbuf_append(&be->code, "\txor r3, r3, r4\n");
            break;
            
        case ANVIL_OP_NOT:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            anvil_strbuf_append(&be->code, "\tnot r3, r3\n");
            break;
            
        case ANVIL_OP_SHL:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
            anvil_strbuf_append(&be->code, "\tslw r3, r3, r4\n");
            break;
            
        case ANVIL_OP_SHR:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
            anvil_strbuf_append(&be->code, "\tsrw r3, r3, r4\n");
            break;
            
        case ANVIL_OP_SAR:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
            anvil_strbuf_append(&be->code, "\tsraw r3, r3, r4\n");
            break;
            
        case ANVIL_OP_PHI:
            /* PHI nodes handled during SSA resolution */
            break;
            
        case ANVIL_OP_ALLOCA:
            {
                int offset = ppc32_add_stack_slot(be, instr->result);
                /* Zero-initialize the slot */
                anvil_strbuf_append(&be->code, "\tli r0, 0\n");
                anvil_strbuf_appendf(&be->code, "\tstw r0, -%d(r31)\n", PPC32_MIN_FRAME_SIZE + offset);
            }
            break;
            
        case ANVIL_OP_LOAD:
            /* Check if loading from stack slot */
            if (instr->operands[0]->kind == ANVIL_VAL_INSTR &&
                instr->operands[0]->data.instr &&
                instr->operands[0]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = ppc32_get_stack_slot(be, instr->operands[0]);
                if (offset >= 0) {
                    anvil_strbuf_appendf(&be->code, "\tlwz r3, -%d(r31)\n", PPC32_MIN_FRAME_SIZE + offset);
                    break;
                }
            }
            /* Check if loading from global */
            if (instr->operands[0]->kind == ANVIL_VAL_GLOBAL) {
                anvil_strbuf_appendf(&be->code, "\tlis r4, %s@ha\n", instr->operands[0]->name);
                anvil_strbuf_appendf(&be->code, "\tlwz r3, %s@l(r4)\n", instr->operands[0]->name);
                break;
            }
            /* Generic load */
            ppc32_emit_load_value(be, instr->operands[0], PPC_R4, func);
            anvil_strbuf_append(&be->code, "\tlwz r3, 0(r4)\n");
            break;
            
        case ANVIL_OP_STORE:
            /* Check if storing to stack slot */
            if (instr->operands[1]->kind == ANVIL_VAL_INSTR &&
                instr->operands[1]->data.instr &&
                instr->operands[1]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = ppc32_get_stack_slot(be, instr->operands[1]);
                if (offset >= 0) {
                    ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
                    anvil_strbuf_appendf(&be->code, "\tstw r3, -%d(r31)\n", PPC32_MIN_FRAME_SIZE + offset);
                    break;
                }
            }
            /* Check if storing to global */
            if (instr->operands[1]->kind == ANVIL_VAL_GLOBAL) {
                ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
                anvil_strbuf_appendf(&be->code, "\tlis r4, %s@ha\n", instr->operands[1]->name);
                anvil_strbuf_appendf(&be->code, "\tstw r3, %s@l(r4)\n", instr->operands[1]->name);
                break;
            }
            /* Generic store */
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
            anvil_strbuf_append(&be->code, "\tstw r3, 0(r4)\n");
            break;
            
        case ANVIL_OP_GEP:
            /* Get Element Pointer - array indexing */
            {
                ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
                
                if (instr->num_operands > 1) {
                    ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
                    
                    int elem_size = 4;
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
                                default:
                                    elem_size = 4;
                                    break;
                            }
                        }
                    }
                    
                    if (elem_size == 1) {
                        anvil_strbuf_append(&be->code, "\tadd r3, r3, r4\n");
                    } else if (elem_size == 2) {
                        anvil_strbuf_append(&be->code, "\tslwi r4, r4, 1\n");
                        anvil_strbuf_append(&be->code, "\tadd r3, r3, r4\n");
                    } else {
                        anvil_strbuf_append(&be->code, "\tslwi r4, r4, 2\n");
                        anvil_strbuf_append(&be->code, "\tadd r3, r3, r4\n");
                    }
                }
            }
            break;
            
        case ANVIL_OP_STRUCT_GEP:
            /* Get Struct Field Pointer */
            {
                ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
                
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
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            anvil_strbuf_append(&be->code, "\tcmpwi cr0, r3, 0\n");
            anvil_strbuf_appendf(&be->code, "\tbne cr0, .L%s_%s\n", func->name, instr->true_block->name);
            anvil_strbuf_appendf(&be->code, "\tb .L%s_%s\n", func->name, instr->false_block->name);
            break;
            
        case ANVIL_OP_RET:
            if (instr->num_operands > 0) {
                ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            }
            ppc32_emit_epilogue(be, func);
            break;
            
        case ANVIL_OP_CALL:
            /* Push arguments to registers (first 8) or stack */
            for (size_t i = 1; i < instr->num_operands && i <= PPC32_NUM_ARG_REGS; i++) {
                ppc32_emit_load_value(be, instr->operands[i], ppc32_arg_regs[i-1], func);
            }
            /* Call function */
            anvil_strbuf_appendf(&be->code, "\tbl %s\n", instr->operands[0]->name);
            break;
            
        case ANVIL_OP_CMP_EQ:
        case ANVIL_OP_CMP_NE:
        case ANVIL_OP_CMP_LT:
        case ANVIL_OP_CMP_LE:
        case ANVIL_OP_CMP_GT:
        case ANVIL_OP_CMP_GE:
            {
                ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
                ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
                anvil_strbuf_append(&be->code, "\tcmpw cr0, r3, r4\n");
                
                /* Set r3 to 1 or 0 based on comparison */
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
                ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
                ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
                anvil_strbuf_append(&be->code, "\tcmplw cr0, r3, r4\n");
                
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
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            if (instr->result && instr->result->type) {
                switch (instr->result->type->kind) {
                    case ANVIL_TYPE_I8:
                    case ANVIL_TYPE_U8:
                        anvil_strbuf_append(&be->code, "\trlwinm r3, r3, 0, 24, 31\n");
                        break;
                    case ANVIL_TYPE_I16:
                    case ANVIL_TYPE_U16:
                        anvil_strbuf_append(&be->code, "\trlwinm r3, r3, 0, 16, 31\n");
                        break;
                    default:
                        break;
                }
            }
            break;
            
        case ANVIL_OP_ZEXT:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            if (instr->operands[0]->type) {
                switch (instr->operands[0]->type->kind) {
                    case ANVIL_TYPE_I8:
                    case ANVIL_TYPE_U8:
                        anvil_strbuf_append(&be->code, "\trlwinm r3, r3, 0, 24, 31\n");
                        break;
                    case ANVIL_TYPE_I16:
                    case ANVIL_TYPE_U16:
                        anvil_strbuf_append(&be->code, "\trlwinm r3, r3, 0, 16, 31\n");
                        break;
                    default:
                        break;
                }
            }
            break;
            
        case ANVIL_OP_SEXT:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            if (instr->operands[0]->type) {
                switch (instr->operands[0]->type->kind) {
                    case ANVIL_TYPE_I8:
                        anvil_strbuf_append(&be->code, "\textsb r3, r3\n");
                        break;
                    case ANVIL_TYPE_I16:
                        anvil_strbuf_append(&be->code, "\textsh r3, r3\n");
                        break;
                    default:
                        break;
                }
            }
            break;
            
        case ANVIL_OP_BITCAST:
        case ANVIL_OP_PTRTOINT:
        case ANVIL_OP_INTTOPTR:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            break;
            
        case ANVIL_OP_SELECT:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
            ppc32_emit_load_value(be, instr->operands[2], PPC_R5, func);
            anvil_strbuf_append(&be->code, "\tcmpwi cr0, r3, 0\n");
            {
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
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            /* Store int to stack, load as FP, convert */
            anvil_strbuf_append(&be->code, "\tstw r3, -8(r1)\n");
            anvil_strbuf_append(&be->code, "\tlis r4, 0x4330\n");
            anvil_strbuf_append(&be->code, "\tstw r4, -4(r1)\n");
            anvil_strbuf_append(&be->code, "\txoris r3, r3, 0x8000\n");
            anvil_strbuf_append(&be->code, "\tstw r3, -8(r1)\n");
            anvil_strbuf_append(&be->code, "\tlfd f1, -8(r1)\n");
            /* Subtract magic number */
            anvil_strbuf_append(&be->code, "\t# Note: requires magic constant in memory\n");
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfrsp f1, f1\n");
            }
            break;
            
        case ANVIL_OP_UITOFP:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            anvil_strbuf_append(&be->code, "\tstw r3, -8(r1)\n");
            anvil_strbuf_append(&be->code, "\tlis r4, 0x4330\n");
            anvil_strbuf_append(&be->code, "\tstw r4, -4(r1)\n");
            anvil_strbuf_append(&be->code, "\tli r4, 0\n");
            anvil_strbuf_append(&be->code, "\tstw r4, -8(r1)\n");
            anvil_strbuf_append(&be->code, "\tlfd f1, -8(r1)\n");
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfrsp f1, f1\n");
            }
            break;
            
        case ANVIL_OP_FPTOSI:
            anvil_strbuf_append(&be->code, "\tfctiwz f1, f1\n");
            anvil_strbuf_append(&be->code, "\tstfd f1, -8(r1)\n");
            anvil_strbuf_append(&be->code, "\tlwz r3, -4(r1)\n");
            break;
            
        case ANVIL_OP_FPTOUI:
            /* PPC32 doesn't have unsigned FP-to-int, use signed and adjust */
            anvil_strbuf_append(&be->code, "\tfctiwz f1, f1\n");
            anvil_strbuf_append(&be->code, "\tstfd f1, -8(r1)\n");
            anvil_strbuf_append(&be->code, "\tlwz r3, -4(r1)\n");
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

static void ppc32_emit_block(ppc32_backend_t *be, anvil_block_t *block, anvil_func_t *func)
{
    if (!block) return;
    
    /* Emit label (skip for entry block) */
    if (block != func->blocks) {
        anvil_strbuf_appendf(&be->code, ".L%s_%s:\n", func->name, block->name);
    }
    
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        ppc32_emit_instr(be, instr, func);
    }
}

static void ppc32_emit_func(ppc32_backend_t *be, anvil_func_t *func)
{
    if (!func || func->is_declaration) return;
    
    be->current_func = func;
    be->num_stack_slots = 0;
    be->next_stack_offset = 0;
    
    /* First pass: count stack slots needed */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_ALLOCA) {
                ppc32_add_stack_slot(be, instr->result);
            }
        }
    }
    
    /* Calculate stack size */
    func->stack_size = PPC32_MIN_FRAME_SIZE + be->next_stack_offset + 32;
    
    /* Reset for actual emission */
    be->num_stack_slots = 0;
    
    ppc32_emit_prologue(be, func);
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        ppc32_emit_block(be, block, func);
    }
    
    anvil_strbuf_appendf(&be->code, "\t.size %s, .-%s\n\n", func->name, func->name);
}

/* Emit global variables */
static void ppc32_emit_globals(ppc32_backend_t *be, anvil_module_t *mod)
{
    if (mod->num_globals == 0) return;
    
    anvil_strbuf_append(&be->data, "\t.data\n");
    
    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        anvil_strbuf_appendf(&be->data, "\t.globl %s\n", g->value->name);
        
        int size = 4;
        int align = 4;
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
                default:
                    size = 4; align = 4;
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
                } else {
                    anvil_strbuf_appendf(&be->data, "\t.long %lld\n", (long long)init->data.i);
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
static void ppc32_emit_strings(ppc32_backend_t *be)
{
    if (be->num_strings == 0) return;
    
    anvil_strbuf_append(&be->data, "\t.section .rodata\n");
    
    for (size_t i = 0; i < be->num_strings; i++) {
        ppc32_string_entry_t *entry = &be->strings[i];
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

static anvil_error_t ppc32_codegen_module(anvil_backend_t *be, anvil_module_t *mod,
                                           char **output, size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;
    
    ppc32_backend_t *priv = be->priv;
    
    /* Reset buffers */
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->label_counter = 0;
    priv->num_strings = 0;
    priv->string_counter = 0;
    
    /* Emit header */
    anvil_strbuf_append(&priv->code, "# Generated by ANVIL for PowerPC 32-bit (big-endian)\n");
    anvil_strbuf_append(&priv->code, "\t.text\n\n");
    
    /* Emit extern declarations */
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (func->is_declaration) {
            anvil_strbuf_appendf(&priv->code, "\t.extern %s\n", func->name);
        }
    }
    
    /* Emit functions */
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (!func->is_declaration) {
            ppc32_emit_func(priv, func);
        }
    }
    
    /* Emit globals */
    ppc32_emit_globals(priv, mod);
    
    /* Emit strings */
    ppc32_emit_strings(priv);
    
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

static anvil_error_t ppc32_codegen_func(anvil_backend_t *be, anvil_func_t *func,
                                         char **output, size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    
    ppc32_backend_t *priv = be->priv;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);
    
    ppc32_emit_func(priv, func);
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

const anvil_backend_ops_t anvil_backend_ppc32 = {
    .name = "PowerPC 32-bit",
    .arch = ANVIL_ARCH_PPC32,
    .init = ppc32_init,
    .cleanup = ppc32_cleanup,
    .codegen_module = ppc32_codegen_module,
    .codegen_func = ppc32_codegen_func,
    .get_arch_info = ppc32_get_arch_info
};
