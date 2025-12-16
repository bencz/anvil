/*
 * ANVIL - PowerPC 64-bit Little-Endian Backend
 * 
 * Little-endian, stack grows downward
 * Generates GAS syntax for PowerPC64 LE
 * 
 * Register conventions (ELFv2 ABI for PPC64LE):
 * - r0: Volatile, used in prologue/epilogue
 * - r1: Stack pointer (SP)
 * - r2: TOC pointer (Table of Contents)
 * - r3-r10: Function arguments and return values
 * - r3: Return value
 * - r11: Environment pointer for nested functions
 * - r12: Volatile, used for linkage (function entry point)
 * - r13: Thread pointer (reserved)
 * - r14-r30: Non-volatile (callee-saved)
 * - r31: Non-volatile, often used as frame pointer
 * - f0: Volatile
 * - f1-f13: Floating-point arguments
 * - f1: Floating-point return value
 * - f14-f31: Non-volatile (callee-saved)
 * - CR0-CR7: Condition registers (CR2-CR4 non-volatile)
 * - LR: Link register (return address)
 * - CTR: Count register
 * 
 * ELFv2 ABI differences from ELFv1:
 * - No function descriptors
 * - Local entry point concept
 * - Minimum frame size: 32 bytes
 * - LR save area at SP+16
 * - TOC save area at SP+24
 * - Parameter save area is optional
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* PowerPC 64-bit register names */
static const char *ppc64le_gpr_names[] = {
    "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
    "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23",
    "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31"
};

/* Register indices */
#define PPC64LE_R0   0
#define PPC64LE_R1   1   /* Stack pointer */
#define PPC64LE_R2   2   /* TOC pointer */
#define PPC64LE_R3   3   /* First arg / return value */
#define PPC64LE_R4   4
#define PPC64LE_R5   5
#define PPC64LE_R6   6
#define PPC64LE_R7   7
#define PPC64LE_R8   8
#define PPC64LE_R9   9
#define PPC64LE_R10  10
#define PPC64LE_R11  11
#define PPC64LE_R12  12  /* Function entry point */
#define PPC64LE_R31  31  /* Frame pointer */

/* Argument registers */
static const int ppc64le_arg_regs[] = { PPC64LE_R3, PPC64LE_R4, PPC64LE_R5, PPC64LE_R6, PPC64LE_R7, PPC64LE_R8, PPC64LE_R9, PPC64LE_R10 };
#define PPC64LE_NUM_ARG_REGS 8

/* ELFv2 ABI constants */
#define PPC64LE_MIN_FRAME_SIZE 32
#define PPC64LE_LR_SAVE_OFFSET 16
#define PPC64LE_TOC_SAVE_OFFSET 24

/* String table entry */
typedef struct {
    const char *str;
    char label[32];
    size_t len;
} ppc64le_string_entry_t;

/* Stack slot tracking */
typedef struct {
    anvil_value_t *value;
    int offset;
} ppc64le_stack_slot_t;

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
    ppc64le_stack_slot_t *stack_slots;
    size_t num_stack_slots;
    size_t stack_slots_cap;
    int next_stack_offset;
    
    /* String table */
    ppc64le_string_entry_t *strings;
    size_t num_strings;
    size_t strings_cap;
    
    /* Current function being generated */
    anvil_func_t *current_func;
    
    /* Context reference */
    anvil_ctx_t *ctx;
} ppc64le_backend_t;

static const anvil_arch_info_t ppc64le_arch_info = {
    .arch = ANVIL_ARCH_PPC64LE,
    .name = "PowerPC 64-bit LE",
    .ptr_size = 8,
    .addr_bits = 64,
    .word_size = 8,
    .num_gpr = 32,
    .num_fpr = 32,
    .endian = ANVIL_ENDIAN_LITTLE,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false
};

/* Forward declarations */
static void ppc64le_emit_func(ppc64le_backend_t *be, anvil_func_t *func);
static void ppc64le_emit_instr(ppc64le_backend_t *be, anvil_instr_t *instr, anvil_func_t *func);

static anvil_error_t ppc64le_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    ppc64le_backend_t *priv = calloc(1, sizeof(ppc64le_backend_t));
    if (!priv) return ANVIL_ERR_NOMEM;
    
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->label_counter = 0;
    priv->ctx = ctx;
    
    be->priv = priv;
    return ANVIL_OK;
}

static void ppc64le_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    ppc64le_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv->strings);
    free(priv->stack_slots);
    free(priv);
    be->priv = NULL;
}

static void ppc64le_reset(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    ppc64le_backend_t *priv = be->priv;
    
    /* Clear stack slots (contain pointers to anvil_value_t) */
    priv->num_stack_slots = 0;
    priv->next_stack_offset = 0;
    priv->stack_offset = 0;
    priv->local_offset = 0;
    
    /* Clear string table (contain pointers to string data) */
    priv->num_strings = 0;
    priv->string_counter = 0;
    
    /* Reset other state */
    priv->label_counter = 0;
    priv->frame_size = 0;
    priv->current_func = NULL;
}

/* Add stack slot for local variable */
static int ppc64le_add_stack_slot(ppc64le_backend_t *be, anvil_value_t *val)
{
    if (be->num_stack_slots >= be->stack_slots_cap) {
        size_t new_cap = be->stack_slots_cap ? be->stack_slots_cap * 2 : 16;
        ppc64le_stack_slot_t *new_slots = realloc(be->stack_slots,
            new_cap * sizeof(ppc64le_stack_slot_t));
        if (!new_slots) return -1;
        be->stack_slots = new_slots;
        be->stack_slots_cap = new_cap;
    }
    
    be->next_stack_offset += 8;
    int offset = be->next_stack_offset;
    
    be->stack_slots[be->num_stack_slots].value = val;
    be->stack_slots[be->num_stack_slots].offset = offset;
    be->num_stack_slots++;
    
    return offset;
}

/* Get stack slot offset for a value */
static int ppc64le_get_stack_slot(ppc64le_backend_t *be, anvil_value_t *val)
{
    for (size_t i = 0; i < be->num_stack_slots; i++) {
        if (be->stack_slots[i].value == val) {
            return be->stack_slots[i].offset;
        }
    }
    return -1;
}

/* Add string to string table */
static const char *ppc64le_add_string(ppc64le_backend_t *be, const char *str)
{
    for (size_t i = 0; i < be->num_strings; i++) {
        if (strcmp(be->strings[i].str, str) == 0) {
            return be->strings[i].label;
        }
    }
    
    if (be->num_strings >= be->strings_cap) {
        size_t new_cap = be->strings_cap ? be->strings_cap * 2 : 16;
        ppc64le_string_entry_t *new_strings = realloc(be->strings,
            new_cap * sizeof(ppc64le_string_entry_t));
        if (!new_strings) return ".str_err";
        be->strings = new_strings;
        be->strings_cap = new_cap;
    }
    
    ppc64le_string_entry_t *entry = &be->strings[be->num_strings];
    entry->str = str;
    entry->len = strlen(str);
    snprintf(entry->label, sizeof(entry->label), ".LC%d", be->string_counter++);
    be->num_strings++;
    
    return entry->label;
}

static const anvil_arch_info_t *ppc64le_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &ppc64le_arch_info;
}

static void ppc64le_emit_prologue(ppc64le_backend_t *be, anvil_func_t *func)
{
    size_t frame_size = func->stack_size;
    if (frame_size < PPC64LE_MIN_FRAME_SIZE) frame_size = PPC64LE_MIN_FRAME_SIZE;
    frame_size = (frame_size + 15) & ~15; /* Align to 16 bytes */
    
    /* ELFv2 ABI - no function descriptors */
    anvil_strbuf_appendf(&be->code, "\t.globl %s\n", func->name);
    anvil_strbuf_appendf(&be->code, "\t.type %s, @function\n", func->name);
    anvil_strbuf_appendf(&be->code, "%s:\n", func->name);
    
    /* Global entry point - set up TOC from r12 */
    anvil_strbuf_appendf(&be->code, "0:\taddis r2, r12, (.TOC.-0b)@ha\n");
    anvil_strbuf_appendf(&be->code, "\taddi r2, r2, (.TOC.-0b)@l\n");
    
    /* Local entry point */
    anvil_strbuf_appendf(&be->code, "\t.localentry %s, .-0b\n", func->name);
    
    /* Save link register */
    anvil_strbuf_append(&be->code, "\tmflr r0\n");
    anvil_strbuf_appendf(&be->code, "\tstd r0, %d(r1)\n", PPC64LE_LR_SAVE_OFFSET);
    
    /* Save callee-saved registers if needed */
    anvil_strbuf_append(&be->code, "\tstd r31, -8(r1)\n");
    
    /* Create stack frame */
    anvil_strbuf_appendf(&be->code, "\tstdu r1, -%zu(r1)\n", frame_size);
    
    /* Set up frame pointer */
    anvil_strbuf_appendf(&be->code, "\taddi r31, r1, %zu\n", frame_size);
    
    be->local_offset = PPC64LE_MIN_FRAME_SIZE;
}

static void ppc64le_emit_epilogue(ppc64le_backend_t *be, anvil_func_t *func)
{
    size_t frame_size = func->stack_size;
    if (frame_size < PPC64LE_MIN_FRAME_SIZE) frame_size = PPC64LE_MIN_FRAME_SIZE;
    frame_size = (frame_size + 15) & ~15;
    
    /* Restore stack pointer */
    anvil_strbuf_appendf(&be->code, "\taddi r1, r1, %zu\n", frame_size);
    
    /* Restore callee-saved registers */
    anvil_strbuf_append(&be->code, "\tld r31, -8(r1)\n");
    
    /* Restore link register and return */
    anvil_strbuf_appendf(&be->code, "\tld r0, %d(r1)\n", PPC64LE_LR_SAVE_OFFSET);
    anvil_strbuf_append(&be->code, "\tmtlr r0\n");
    anvil_strbuf_append(&be->code, "\tblr\n");
}

static void ppc64le_emit_load_value(ppc64le_backend_t *be, anvil_value_t *val, int reg, anvil_func_t *func)
{
    if (!val) return;
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_INT:
            if (val->data.i >= -32768 && val->data.i <= 32767) {
                anvil_strbuf_appendf(&be->code, "\tli %s, %lld\n",
                    ppc64le_gpr_names[reg], (long long)val->data.i);
            } else if (val->data.i >= -2147483648LL && val->data.i <= 2147483647LL) {
                /* 32-bit immediate */
                anvil_strbuf_appendf(&be->code, "\tlis %s, %lld@ha\n",
                    ppc64le_gpr_names[reg], (long long)((val->data.i >> 16) & 0xFFFF));
                anvil_strbuf_appendf(&be->code, "\tori %s, %s, %lld@l\n",
                    ppc64le_gpr_names[reg], ppc64le_gpr_names[reg], 
                    (long long)(val->data.i & 0xFFFF));
            } else {
                /* Full 64-bit immediate */
                int64_t v = val->data.i;
                anvil_strbuf_appendf(&be->code, "\tlis %s, %lld\n",
                    ppc64le_gpr_names[reg], (long long)((v >> 48) & 0xFFFF));
                anvil_strbuf_appendf(&be->code, "\tori %s, %s, %lld\n",
                    ppc64le_gpr_names[reg], ppc64le_gpr_names[reg], 
                    (long long)((v >> 32) & 0xFFFF));
                anvil_strbuf_appendf(&be->code, "\tsldi %s, %s, 32\n",
                    ppc64le_gpr_names[reg], ppc64le_gpr_names[reg]);
                anvil_strbuf_appendf(&be->code, "\toris %s, %s, %lld\n",
                    ppc64le_gpr_names[reg], ppc64le_gpr_names[reg],
                    (long long)((v >> 16) & 0xFFFF));
                anvil_strbuf_appendf(&be->code, "\tori %s, %s, %lld\n",
                    ppc64le_gpr_names[reg], ppc64le_gpr_names[reg],
                    (long long)(v & 0xFFFF));
            }
            break;
            
        case ANVIL_VAL_PARAM:
            {
                size_t idx = val->data.param.index;
                if (idx < PPC64LE_NUM_ARG_REGS) {
                    if (ppc64le_arg_regs[idx] != reg) {
                        anvil_strbuf_appendf(&be->code, "\tmr %s, %s\n",
                            ppc64le_gpr_names[reg], ppc64le_gpr_names[ppc64le_arg_regs[idx]]);
                    }
                } else {
                    /* Parameters on stack (ELFv2: no mandatory save area) */
                    size_t offset = PPC64LE_MIN_FRAME_SIZE + (idx - PPC64LE_NUM_ARG_REGS) * 8;
                    anvil_strbuf_appendf(&be->code, "\tld %s, %zu(r31)\n",
                        ppc64le_gpr_names[reg], offset);
                }
            }
            break;
            
        case ANVIL_VAL_CONST_NULL:
            anvil_strbuf_appendf(&be->code, "\tli %s, 0\n", ppc64le_gpr_names[reg]);
            break;
            
        case ANVIL_VAL_CONST_STRING:
            {
                const char *label = ppc64le_add_string(be, val->data.str ? val->data.str : "");
                anvil_strbuf_appendf(&be->code, "\taddis %s, r2, %s@toc@ha\n",
                    ppc64le_gpr_names[reg], label);
                anvil_strbuf_appendf(&be->code, "\taddi %s, %s, %s@toc@l\n",
                    ppc64le_gpr_names[reg], ppc64le_gpr_names[reg], label);
            }
            break;
            
        case ANVIL_VAL_INSTR:
            if (val->data.instr && val->data.instr->op == ANVIL_OP_ALLOCA) {
                /* Load address of stack slot */
                int offset = ppc64le_get_stack_slot(be, val);
                if (offset >= 0) {
                    anvil_strbuf_appendf(&be->code, "\taddi %s, r31, -%d\n",
                        ppc64le_gpr_names[reg], PPC64LE_MIN_FRAME_SIZE + offset);
                }
            } else {
                /* Result should be in r3 by convention */
                if (reg != PPC64LE_R3) {
                    anvil_strbuf_appendf(&be->code, "\tmr %s, r3\n", ppc64le_gpr_names[reg]);
                }
            }
            break;
            
        case ANVIL_VAL_FUNC:
            /* Load function address via TOC */
            anvil_strbuf_appendf(&be->code, "\taddis %s, r2, %s@toc@ha\n",
                ppc64le_gpr_names[reg], val->name);
            anvil_strbuf_appendf(&be->code, "\tld %s, %s@toc@l(%s)\n",
                ppc64le_gpr_names[reg], val->name, ppc64le_gpr_names[reg]);
            break;
            
        case ANVIL_VAL_GLOBAL:
            anvil_strbuf_appendf(&be->code, "\taddis %s, r2, %s@toc@ha\n",
                ppc64le_gpr_names[reg], val->name);
            anvil_strbuf_appendf(&be->code, "\tld %s, %s@toc@l(%s)\n",
                ppc64le_gpr_names[reg], val->name, ppc64le_gpr_names[reg]);
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "\t# unhandled value kind %d\n", val->kind);
            break;
    }
    
    (void)func;
}

static void ppc64le_emit_instr(ppc64le_backend_t *be, anvil_instr_t *instr, anvil_func_t *func)
{
    if (!instr) return;
    
    switch (instr->op) {
        case ANVIL_OP_ADD:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
            anvil_strbuf_append(&be->code, "\tadd r3, r3, r4\n");
            break;
            
        case ANVIL_OP_SUB:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
            anvil_strbuf_append(&be->code, "\tsub r3, r3, r4\n");
            break;
            
        case ANVIL_OP_MUL:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
            anvil_strbuf_append(&be->code, "\tmulld r3, r3, r4\n");
            break;
            
        case ANVIL_OP_SDIV:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
            anvil_strbuf_append(&be->code, "\tdivd r3, r3, r4\n");
            break;
            
        case ANVIL_OP_UDIV:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
            anvil_strbuf_append(&be->code, "\tdivdu r3, r3, r4\n");
            break;
            
        case ANVIL_OP_SMOD:
        case ANVIL_OP_UMOD:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
            if (instr->op == ANVIL_OP_SMOD) {
                anvil_strbuf_append(&be->code, "\tdivd r5, r3, r4\n");
            } else {
                anvil_strbuf_append(&be->code, "\tdivdu r5, r3, r4\n");
            }
            anvil_strbuf_append(&be->code, "\tmulld r5, r5, r4\n");
            anvil_strbuf_append(&be->code, "\tsub r3, r3, r5\n");
            break;
            
        case ANVIL_OP_NEG:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            anvil_strbuf_append(&be->code, "\tneg r3, r3\n");
            break;
            
        case ANVIL_OP_AND:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
            anvil_strbuf_append(&be->code, "\tand r3, r3, r4\n");
            break;
            
        case ANVIL_OP_OR:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
            anvil_strbuf_append(&be->code, "\tor r3, r3, r4\n");
            break;
            
        case ANVIL_OP_XOR:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
            anvil_strbuf_append(&be->code, "\txor r3, r3, r4\n");
            break;
            
        case ANVIL_OP_NOT:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            anvil_strbuf_append(&be->code, "\tnot r3, r3\n");
            break;
            
        case ANVIL_OP_SHL:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
            anvil_strbuf_append(&be->code, "\tsld r3, r3, r4\n");
            break;
            
        case ANVIL_OP_SHR:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
            anvil_strbuf_append(&be->code, "\tsrd r3, r3, r4\n");
            break;
            
        case ANVIL_OP_SAR:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
            anvil_strbuf_append(&be->code, "\tsrad r3, r3, r4\n");
            break;
            
        case ANVIL_OP_PHI:
            /* PHI nodes handled during SSA resolution */
            break;
            
        case ANVIL_OP_ALLOCA:
            {
                int offset = ppc64le_add_stack_slot(be, instr->result);
                /* Zero-initialize the slot */
                anvil_strbuf_append(&be->code, "\tli r0, 0\n");
                anvil_strbuf_appendf(&be->code, "\tstd r0, -%d(r31)\n", PPC64LE_MIN_FRAME_SIZE + offset);
            }
            break;
            
        case ANVIL_OP_LOAD:
            /* Check if loading from stack slot */
            if (instr->operands[0]->kind == ANVIL_VAL_INSTR &&
                instr->operands[0]->data.instr &&
                instr->operands[0]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = ppc64le_get_stack_slot(be, instr->operands[0]);
                if (offset >= 0) {
                    anvil_strbuf_appendf(&be->code, "\tld r3, -%d(r31)\n", PPC64LE_MIN_FRAME_SIZE + offset);
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
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R4, func);
            anvil_strbuf_append(&be->code, "\tld r3, 0(r4)\n");
            break;
            
        case ANVIL_OP_STORE:
            /* Check if storing to stack slot */
            if (instr->operands[1]->kind == ANVIL_VAL_INSTR &&
                instr->operands[1]->data.instr &&
                instr->operands[1]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = ppc64le_get_stack_slot(be, instr->operands[1]);
                if (offset >= 0) {
                    ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
                    anvil_strbuf_appendf(&be->code, "\tstd r3, -%d(r31)\n", PPC64LE_MIN_FRAME_SIZE + offset);
                    break;
                }
            }
            /* Check if storing to global */
            if (instr->operands[1]->kind == ANVIL_VAL_GLOBAL) {
                ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
                anvil_strbuf_appendf(&be->code, "\taddis r4, r2, %s@toc@ha\n", instr->operands[1]->name);
                anvil_strbuf_appendf(&be->code, "\tstd r3, %s@toc@l(r4)\n", instr->operands[1]->name);
                break;
            }
            /* Generic store */
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
            anvil_strbuf_append(&be->code, "\tstd r3, 0(r4)\n");
            break;
            
        case ANVIL_OP_GEP:
            /* Get Element Pointer - array indexing */
            {
                ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
                
                if (instr->num_operands > 1) {
                    ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
                    
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
                ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
                
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
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            anvil_strbuf_append(&be->code, "\tcmpdi cr0, r3, 0\n");
            anvil_strbuf_appendf(&be->code, "\tbne cr0, .L%s_%s\n", func->name, instr->true_block->name);
            anvil_strbuf_appendf(&be->code, "\tb .L%s_%s\n", func->name, instr->false_block->name);
            break;
            
        case ANVIL_OP_RET:
            if (instr->num_operands > 0) {
                ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            }
            ppc64le_emit_epilogue(be, func);
            break;
            
        case ANVIL_OP_CALL:
            for (size_t i = 1; i < instr->num_operands && i <= PPC64LE_NUM_ARG_REGS; i++) {
                ppc64le_emit_load_value(be, instr->operands[i], ppc64le_arg_regs[i-1], func);
            }
            /* ELFv2: simpler call sequence */
            anvil_strbuf_appendf(&be->code, "\tbl %s\n", instr->operands[0]->name);
            anvil_strbuf_append(&be->code, "\tnop\n"); /* TOC restore hint */
            break;
            
        case ANVIL_OP_CMP_EQ:
        case ANVIL_OP_CMP_NE:
        case ANVIL_OP_CMP_LT:
        case ANVIL_OP_CMP_LE:
        case ANVIL_OP_CMP_GT:
        case ANVIL_OP_CMP_GE:
            {
                ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
                ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
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
                ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
                ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
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
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
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
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
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
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
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
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            break;
            
        case ANVIL_OP_SELECT:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            ppc64le_emit_load_value(be, instr->operands[1], PPC64LE_R4, func);
            ppc64le_emit_load_value(be, instr->operands[2], PPC64LE_R5, func);
            anvil_strbuf_append(&be->code, "\tcmpdi cr0, r3, 0\n");
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
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
            anvil_strbuf_append(&be->code, "\tstd r3, -8(r1)\n");
            anvil_strbuf_append(&be->code, "\tlfd f1, -8(r1)\n");
            anvil_strbuf_append(&be->code, "\tfcfid f1, f1\n");
            if (instr->result && instr->result->type &&
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "\tfrsp f1, f1\n");
            }
            break;
            
        case ANVIL_OP_UITOFP:
            ppc64le_emit_load_value(be, instr->operands[0], PPC64LE_R3, func);
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

static void ppc64le_emit_block(ppc64le_backend_t *be, anvil_block_t *block, anvil_func_t *func)
{
    if (!block) return;
    
    /* Emit label (skip for entry block) */
    if (block != func->blocks) {
        anvil_strbuf_appendf(&be->code, ".L%s_%s:\n", func->name, block->name);
    }
    
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        ppc64le_emit_instr(be, instr, func);
    }
}

static void ppc64le_emit_func(ppc64le_backend_t *be, anvil_func_t *func)
{
    if (!func || func->is_declaration) return;
    
    be->current_func = func;
    be->num_stack_slots = 0;
    be->next_stack_offset = 0;
    
    /* First pass: count stack slots needed */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_ALLOCA) {
                ppc64le_add_stack_slot(be, instr->result);
            }
        }
    }
    
    /* Calculate stack size */
    func->stack_size = PPC64LE_MIN_FRAME_SIZE + be->next_stack_offset + 64;
    
    /* Reset for actual emission */
    be->num_stack_slots = 0;
    
    ppc64le_emit_prologue(be, func);
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        ppc64le_emit_block(be, block, func);
    }
    
    anvil_strbuf_appendf(&be->code, "\t.size %s, .-%s\n\n", func->name, func->name);
}

/* Emit global variables */
static void ppc64le_emit_globals(ppc64le_backend_t *be, anvil_module_t *mod)
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

/* Emit string constants */
static void ppc64le_emit_strings(ppc64le_backend_t *be)
{
    if (be->num_strings == 0) return;
    
    anvil_strbuf_append(&be->data, "\t.section .rodata\n");
    
    for (size_t i = 0; i < be->num_strings; i++) {
        ppc64le_string_entry_t *entry = &be->strings[i];
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

static anvil_error_t ppc64le_codegen_module(anvil_backend_t *be, anvil_module_t *mod,
                                             char **output, size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;
    
    ppc64le_backend_t *priv = be->priv;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->label_counter = 0;
    priv->num_strings = 0;
    priv->string_counter = 0;
    
    /* Emit header */
    anvil_strbuf_append(&priv->code, "# Generated by ANVIL for PowerPC 64-bit (little-endian, ELFv2 ABI)\n");
    anvil_strbuf_append(&priv->code, "\t.abiversion 2\n");
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
            ppc64le_emit_func(priv, func);
        }
    }
    
    /* Emit globals */
    ppc64le_emit_globals(priv, mod);
    
    /* Emit strings */
    ppc64le_emit_strings(priv);
    
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

static anvil_error_t ppc64le_codegen_func(anvil_backend_t *be, anvil_func_t *func,
                                           char **output, size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    
    ppc64le_backend_t *priv = be->priv;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);
    
    ppc64le_emit_func(priv, func);
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

const anvil_backend_ops_t anvil_backend_ppc64le = {
    .name = "PowerPC 64-bit LE",
    .arch = ANVIL_ARCH_PPC64LE,
    .init = ppc64le_init,
    .cleanup = ppc64le_cleanup,
    .reset = ppc64le_reset,
    .codegen_module = ppc64le_codegen_module,
    .codegen_func = ppc64le_codegen_func,
    .get_arch_info = ppc64le_get_arch_info
};
