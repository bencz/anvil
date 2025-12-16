/*
 * ANVIL - IBM z/Architecture Backend
 * 
 * Big-endian, stack grows UPWARD (toward higher addresses)
 * 64-bit addressing mode
 * Generates HLASM (High Level Assembler) syntax
 * 
 * z/Architecture features:
 *   - 64-bit general purpose registers
 *   - 64-bit addressing
 *   - Extended instruction set (LLGF, LGR, AGR, MSGR, etc.)
 *   - Long displacement facility
 *   - Relative-long instructions (LGRL, BRASL, etc.)
 *   - LGHI, LGFI (Load immediate)
 * 
 * Register conventions (z/OS 64-bit linkage):
 *   R0      - Work register (volatile)
 *   R1      - Parameter list pointer (points to list of addresses)
 *   R2-R11  - General purpose / work registers
 *   R12     - Base register for addressability
 *   R13     - Save area pointer
 *   R14     - Return address
 *   R15     - Entry point address / return code
 * 
 * 64-bit save area format (F4SA - 144 bytes = 18 doublewords):
 *   +0    - Reserved
 *   +8    - Pointer to previous save area (caller's SA)
 *   +16   - Pointer to next save area (callee's SA)
 *   +24   - R14 (return address)
 *   +32   - R15 (entry point)
 *   +40   - R0
 *   +48   - R1
 *   ...   - ...
 *   +136  - R12
 * 
 * Parameter passing (z/OS 64-bit):
 *   R1 points to a list of doubleword addresses
 *   Each address points to the actual parameter value
 *   High-order bit of last address is set to 1
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* z/Architecture registers */
static const char *zarch_reg_names[] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
    "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"
};

/* z/Architecture has 16 FPRs (0-15) - supports both HFP and IEEE 754 */
#define ZARCH_F0  0
#define ZARCH_F2  2
#define ZARCH_F4  4
#define ZARCH_F6  6

/* Register usage */
#define ZARCH_R0   0
#define ZARCH_R1   1
#define ZARCH_R2   2
#define ZARCH_R3   3
#define ZARCH_R12  12
#define ZARCH_R13  13
#define ZARCH_R14  14
#define ZARCH_R15  15

/* 64-bit save area size (F4SA format) */
#define SA64_SIZE  144

/* Dynamic storage layout (relative to R13):
 * +0    - Save Area (144 bytes for 64-bit)
 * +144  - FP temp area (8 bytes for double)
 * +152  - FP temp area 2 (8 bytes for conversions)
 * +160  - Local variables start
 * +N    - Parameter list for outgoing calls (8 bytes each)
 */
#define FP_TEMP_OFFSET     144
#define FP_TEMP2_OFFSET    152
#define DYN_LOCALS_OFFSET  160

/* String table entry */
typedef struct {
    const char *str;
    char label[16];
    size_t len;
} zarch_string_entry_t;

/* Backend private data */
typedef struct {
    anvil_strbuf_t code;
    anvil_strbuf_t data;
    int label_counter;
    int literal_counter;
    int string_counter;
    int local_vars_size;
    int max_call_args;
    const char *csect_name;
    const char *current_func;
    anvil_ctx_t *ctx;  /* Context for FP format selection */
    
    /* Stack slots for local variables */
    struct {
        anvil_value_t *value;
        int offset;
    } *stack_slots;
    size_t num_stack_slots;
    size_t stack_slots_cap;
    
    /* String table */
    zarch_string_entry_t *strings;
    size_t num_strings;
    size_t strings_cap;
} zarch_backend_t;

static const anvil_arch_info_t zarch_arch_info = {
    .arch = ANVIL_ARCH_ZARCH,
    .name = "z/Architecture",
    .ptr_size = 8,
    .addr_bits = 64,
    .word_size = 8,
    .num_gpr = 16,
    .num_fpr = 16,
    .endian = ANVIL_ENDIAN_BIG,
    .stack_dir = ANVIL_STACK_UP,
    .has_condition_codes = true,
    .has_delay_slots = false
};

static anvil_error_t zarch_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    zarch_backend_t *priv = calloc(1, sizeof(zarch_backend_t));
    if (!priv) return ANVIL_ERR_NOMEM;
    
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->csect_name = "CODE";
    priv->ctx = ctx;  /* Save context for FP format selection */
    
    be->priv = priv;
    return ANVIL_OK;
}

static void zarch_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    zarch_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv->strings);
    free(priv->stack_slots);
    free(priv);
    be->priv = NULL;
}

static void zarch_reset(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    zarch_backend_t *priv = be->priv;
    
    /* Clear stack slots (contain pointers to anvil_value_t) */
    priv->num_stack_slots = 0;
    priv->local_vars_size = 0;
    
    /* Clear string table (contain pointers to string data) */
    priv->num_strings = 0;
    priv->string_counter = 0;
    
    /* Reset other state */
    priv->label_counter = 0;
    priv->literal_counter = 0;
    priv->max_call_args = 0;
    priv->current_func = NULL;
}

/* Get stack slot offset for an ALLOCA result value */
static int zarch_get_stack_slot(zarch_backend_t *be, anvil_value_t *val)
{
    for (size_t i = 0; i < be->num_stack_slots; i++) {
        if (be->stack_slots[i].value == val) {
            return be->stack_slots[i].offset;
        }
    }
    return -1;
}

/* Add a stack slot for an ALLOCA */
static int zarch_add_stack_slot(zarch_backend_t *be, anvil_value_t *val)
{
    if (be->num_stack_slots >= be->stack_slots_cap) {
        size_t new_cap = be->stack_slots_cap ? be->stack_slots_cap * 2 : 16;
        void *new_slots = realloc(be->stack_slots, new_cap * sizeof(be->stack_slots[0]));
        if (!new_slots) return -1;
        be->stack_slots = new_slots;
        be->stack_slots_cap = new_cap;
    }
    
    int offset = DYN_LOCALS_OFFSET + be->local_vars_size;
    be->stack_slots[be->num_stack_slots].value = val;
    be->stack_slots[be->num_stack_slots].offset = offset;
    be->num_stack_slots++;
    be->local_vars_size += 8;  /* 8 bytes for 64-bit */
    
    return offset;
}

static const anvil_arch_info_t *zarch_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &zarch_arch_info;
}

/* Convert function name to uppercase (GCCMVS convention) */
static void zarch_uppercase(char *dest, const char *src, size_t max_len)
{
    size_t i;
    for (i = 0; i < max_len - 1 && src[i]; i++) {
        dest[i] = (src[i] >= 'a' && src[i] <= 'z') ? src[i] - 32 : src[i];
    }
    dest[i] = '\0';
}

static void zarch_emit_header(zarch_backend_t *be, const char *module_name)
{
    (void)module_name;
    anvil_strbuf_append(&be->code, "***********************************************************************\n");
    anvil_strbuf_appendf(&be->code, "*        Generated by ANVIL for IBM z/Architecture\n");
    anvil_strbuf_append(&be->code, "***********************************************************************\n");
    anvil_strbuf_append(&be->code, "         CSECT\n");
    anvil_strbuf_append(&be->code, "         AMODE ANY\n");
    anvil_strbuf_append(&be->code, "         RMODE ANY\n");
    anvil_strbuf_append(&be->code, "*\n");
}

static void zarch_emit_prologue(zarch_backend_t *be, anvil_func_t *func)
{
    char upper_name[64];
    zarch_uppercase(upper_name, func->name, sizeof(upper_name));
    be->current_func = func->name;
    
    /* Entry point label (uppercase) */
    anvil_strbuf_appendf(&be->code, "%-8s DS    0H\n", upper_name);
    
    /* 1. Save caller's registers using STMG (Store Multiple 64-bit)
     *    F4SA layout: R14 at +24, R15 at +32, R0 at +40, etc. */
    anvil_strbuf_append(&be->code, "         STMG  R14,R12,24(R13)    Save caller's registers\n");
    
    /* 2. Establish addressability */
    anvil_strbuf_append(&be->code, "         LGR   R12,R15            Copy entry point to base reg\n");
    anvil_strbuf_appendf(&be->code, "         USING %s,R12            Establish addressability\n", upper_name);
    
    /* 3. Save R1 (param pointer) */
    anvil_strbuf_append(&be->code, "         LGR   R11,R1             Save parameter list pointer\n");
    
    /* 4. Set up save area chain (stack allocation, no STORAGE OBTAIN) */
    anvil_strbuf_append(&be->code, "*        Set up save area chain (stack allocation)\n");
    anvil_strbuf_append(&be->code, "         LA    R2,144(,R13)       R2 -> our save area (144 bytes for 64-bit SA)\n");
    anvil_strbuf_append(&be->code, "         STG   R13,8(,R2)         Chain: new->prev = caller's\n");
    anvil_strbuf_append(&be->code, "         STG   R2,16(,R13)        Chain: caller->next = new\n");
    anvil_strbuf_append(&be->code, "         LGR   R13,R2             R13 -> our save area\n");
    anvil_strbuf_append(&be->code, "*\n");
}

static void zarch_emit_epilogue(zarch_backend_t *be)
{
    (void)be;
    anvil_strbuf_append(&be->code, "*        Function epilogue\n");
    
    /* 1. Restore caller's SA pointer */
    anvil_strbuf_append(&be->code, "         LG    R13,8(,R13)        Restore caller's SA pointer\n");
    
    /* 2. Restore registers - R15 has return value
     *    64-bit SA: R14 at +24, R0 at +40 */
    anvil_strbuf_append(&be->code, "         LG    R14,24(,R13)       Restore return address\n");
    anvil_strbuf_append(&be->code, "         LMG   R0,R12,40(,R13)    Restore R0-R12\n");
    anvil_strbuf_append(&be->code, "         BR    R14                Return to caller\n");
}

/* Add string to string table and return its label */
static const char *zarch_add_string(zarch_backend_t *be, const char *str)
{
    /* Check if string already exists */
    for (size_t i = 0; i < be->num_strings; i++) {
        if (strcmp(be->strings[i].str, str) == 0) {
            return be->strings[i].label;
        }
    }
    
    /* Grow table if needed */
    if (be->num_strings >= be->strings_cap) {
        size_t new_cap = be->strings_cap ? be->strings_cap * 2 : 16;
        zarch_string_entry_t *new_strings = realloc(be->strings, 
            new_cap * sizeof(zarch_string_entry_t));
        if (!new_strings) return "STR$ERR";
        be->strings = new_strings;
        be->strings_cap = new_cap;
    }
    
    /* Add new string */
    zarch_string_entry_t *entry = &be->strings[be->num_strings];
    entry->str = str;
    entry->len = strlen(str);
    snprintf(entry->label, sizeof(entry->label), "STR$%d", be->string_counter++);
    be->num_strings++;
    
    return entry->label;
}

/* Emit floating-point value to FP register (HFP or IEEE based on context) */
static void zarch_emit_load_fp_value(zarch_backend_t *be, anvil_value_t *val, int target_fpr)
{
    if (!val) return;
    
    bool use_ieee = (be->ctx && (be->ctx->fp_format == ANVIL_FP_IEEE754 || 
                                  be->ctx->fp_format == ANVIL_FP_HFP_IEEE));
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_FLOAT:
            if (val->type && val->type->kind == ANVIL_TYPE_F32) {
                if (use_ieee) {
                    /* IEEE short - use LE with IEEE literal (EB format) */
                    anvil_strbuf_appendf(&be->code, "         LE    %d,=EB'%g'        Load IEEE short FP\n",
                        target_fpr, val->data.f);
                } else {
                    /* HFP short */
                    anvil_strbuf_appendf(&be->code, "         LE    %d,=E'%g'         Load HFP short FP\n",
                        target_fpr, val->data.f);
                }
            } else {
                if (use_ieee) {
                    /* IEEE long - use LD with IEEE literal (DB format) */
                    anvil_strbuf_appendf(&be->code, "         LD    %d,=DB'%g'        Load IEEE long FP\n",
                        target_fpr, val->data.f);
                } else {
                    /* HFP long */
                    anvil_strbuf_appendf(&be->code, "         LD    %d,=D'%g'         Load HFP long FP\n",
                        target_fpr, val->data.f);
                }
            }
            break;
            
        case ANVIL_VAL_INSTR:
            if (target_fpr != ZARCH_F0) {
                if (val->type && val->type->kind == ANVIL_TYPE_F32) {
                    anvil_strbuf_appendf(&be->code, "         LER   %d,0             Copy short FP result\n", target_fpr);
                } else {
                    anvil_strbuf_appendf(&be->code, "         LDR   %d,0             Copy long FP result\n", target_fpr);
                }
            }
            break;
            
        case ANVIL_VAL_PARAM:
            /* FP parameter - load from parameter area (64-bit addresses) */
            anvil_strbuf_appendf(&be->code, "         LG    R2,%zu(,R11)       Load addr of FP param %zu\n",
                val->data.param.index * 8, val->data.param.index);
            anvil_strbuf_append(&be->code, "         NIHH  R2,X'7FFF'        Clear VL bit\n");
            if (val->type && val->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_appendf(&be->code, "         LE    %d,0(,R2)         Load short FP param\n", target_fpr);
            } else {
                anvil_strbuf_appendf(&be->code, "         LD    %d,0(,R2)         Load long FP param\n", target_fpr);
            }
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "*        Unknown FP value kind %d\n", val->kind);
            break;
    }
}

static void zarch_emit_load_value(zarch_backend_t *be, anvil_value_t *val, int target_reg)
{
    if (!val) return;
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_INT:
            if (val->data.i >= -32768 && val->data.i <= 32767) {
                /* LGHI - Load 64-bit Halfword Immediate */
                anvil_strbuf_appendf(&be->code, "         LGHI  %s,%lld           Load constant\n",
                    zarch_reg_names[target_reg], (long long)val->data.i);
            } else if (val->data.i >= -2147483648LL && val->data.i <= 2147483647LL) {
                /* LGFI - Load 64-bit Fullword Immediate */
                anvil_strbuf_appendf(&be->code, "         LGFI  %s,%lld          Load constant\n",
                    zarch_reg_names[target_reg], (long long)val->data.i);
            } else {
                /* Use literal for 64-bit constants */
                anvil_strbuf_appendf(&be->code, "         LG    %s,=FD'%lld'     Load constant\n",
                    zarch_reg_names[target_reg], (long long)val->data.i);
            }
            break;
            
        case ANVIL_VAL_CONST_STRING:
            {
                /* Add string to table and load its address using LARL */
                const char *label = zarch_add_string(be, val->data.str ? val->data.str : "");
                anvil_strbuf_appendf(&be->code, "         LARL  %s,%s            Load string address\n",
                    zarch_reg_names[target_reg], label);
            }
            break;
            
        case ANVIL_VAL_PARAM:
            /* z/OS 64-bit parameter passing: R11 has saved R1 (param list pointer) */
            /* R1 points to list of ADDRESSES (8 bytes each), each address points to the value */
            /* Step 1: Load address of parameter from list */
            anvil_strbuf_appendf(&be->code, "         LG    %s,%zu(,R11)       Load addr of param %zu\n",
                zarch_reg_names[target_reg], val->data.param.index * 8, val->data.param.index);
            /* Note: We do NOT clear the VL bit - allows full 64-bit addressing */
            /* Step 2: Load actual value from that address */
            anvil_strbuf_appendf(&be->code, "         LG    %s,0(,%s)         Load param value\n",
                zarch_reg_names[target_reg], zarch_reg_names[target_reg]);
            break;
            
        case ANVIL_VAL_INSTR:
            /* Check if this is an ALLOCA result - use stack slot offset (address) */
            if (val->data.instr && val->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = zarch_get_stack_slot(be, val);
                if (offset >= 0) {
                    anvil_strbuf_appendf(&be->code, "         LA    %s,%d(,R13)       Load addr of local var\n",
                        zarch_reg_names[target_reg], offset);
                    break;
                }
            }
            /* Check if this is a LOAD from a stack slot - load value directly */
            if (val->data.instr && val->data.instr->op == ANVIL_OP_LOAD) {
                anvil_instr_t *load_instr = val->data.instr;
                if (load_instr->operands[0]->kind == ANVIL_VAL_INSTR &&
                    load_instr->operands[0]->data.instr &&
                    load_instr->operands[0]->data.instr->op == ANVIL_OP_ALLOCA) {
                    int offset = zarch_get_stack_slot(be, load_instr->operands[0]);
                    if (offset >= 0) {
                        anvil_strbuf_appendf(&be->code, "         LG    %s,%d(,R13)       Load value from stack slot\n",
                            zarch_reg_names[target_reg], offset);
                        break;
                    }
                }
            }
            /* Otherwise, result in R15 by convention */
            if (target_reg != ZARCH_R15) {
                anvil_strbuf_appendf(&be->code, "         LGR   %s,R15            Copy result\n",
                    zarch_reg_names[target_reg]);
            }
            break;
            
        case ANVIL_VAL_GLOBAL:
            /* Load global value - use LARL for address, LGRL for value */
            {
                char upper_name[64];
                zarch_uppercase(upper_name, val->name, sizeof(upper_name));
                if (val->type && val->type->kind == ANVIL_TYPE_PTR) {
                    /* Load address of global using relative long */
                    anvil_strbuf_appendf(&be->code, "         LARL  %s,%s            Load global address\n",
                        zarch_reg_names[target_reg], upper_name);
                } else {
                    /* Load value from global using relative long */
                    anvil_strbuf_appendf(&be->code, "         LGRL  %s,%s            Load global value\n",
                        zarch_reg_names[target_reg], upper_name);
                }
            }
            break;
            
        case ANVIL_VAL_FUNC:
            /* Load function entry point address using relative long */
            anvil_strbuf_appendf(&be->code, "         LARL  %s,%s            Load function address\n",
                zarch_reg_names[target_reg], val->name);
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "*        Unknown value kind %d\n", val->kind);
            break;
    }
}

static void zarch_emit_instr(zarch_backend_t *be, anvil_instr_t *instr)
{
    if (!instr) return;
    
    switch (instr->op) {
        case ANVIL_OP_PHI:
            /* PHI nodes are SSA abstractions - value already in R15 from predecessor */
            break;
            
        case ANVIL_OP_ALLOCA:
            /* Allocate space in dynamic area for local variable */
            {
                int offset = zarch_add_stack_slot(be, instr->result);
                anvil_strbuf_appendf(&be->code, "         XC    %d(8,R13),%d(R13)  Init local var to 0\n", 
                    offset, offset);
            }
            break;
            
        case ANVIL_OP_ADD:
            /* Optimization: use AGHI for small immediate constants */
            if (instr->operands[1]->kind == ANVIL_VAL_CONST_INT &&
                instr->operands[1]->data.i >= -32768 && instr->operands[1]->data.i <= 32767) {
                zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
                anvil_strbuf_appendf(&be->code, "         AGHI  R2,%lld            Add halfword immediate 64-bit\n",
                    (long long)instr->operands[1]->data.i);
                anvil_strbuf_append(&be->code, "         LGR   R15,R2            Result in R15\n");
            } else {
                zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
                zarch_emit_load_value(be, instr->operands[1], ZARCH_R3);
                anvil_strbuf_append(&be->code, "         AGR   R2,R3             Add 64-bit registers\n");
                anvil_strbuf_append(&be->code, "         LGR   R15,R2            Result in R15\n");
            }
            break;
            
        case ANVIL_OP_SUB:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            zarch_emit_load_value(be, instr->operands[1], ZARCH_R3);
            /* SGR - Subtract 64-bit registers */
            anvil_strbuf_append(&be->code, "         SGR   R2,R3             Subtract 64-bit registers\n");
            anvil_strbuf_append(&be->code, "         LGR   R15,R2            Result in R15\n");
            break;
            
        case ANVIL_OP_MUL:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            zarch_emit_load_value(be, instr->operands[1], ZARCH_R3);
            /* MSGR - Multiply Single 64-bit */
            anvil_strbuf_append(&be->code, "         MSGR  R2,R3             Multiply single 64-bit\n");
            anvil_strbuf_append(&be->code, "         LGR   R15,R2            Result in R15\n");
            break;
            
        case ANVIL_OP_SDIV:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R3);
            /* Sign extend into R2:R3 pair */
            anvil_strbuf_append(&be->code, "         LGR   R2,R3             Copy to R2\n");
            anvil_strbuf_append(&be->code, "         SRAG  R2,R2,63          Sign extend into R2\n");
            zarch_emit_load_value(be, instr->operands[1], ZARCH_R0);
            /* DSGR - Divide Single 64-bit */
            anvil_strbuf_append(&be->code, "         DSGR  R2,R0             Divide R2:R3 by R0\n");
            anvil_strbuf_append(&be->code, "         LGR   R15,R3            Quotient to R15\n");
            break;
            
        case ANVIL_OP_SMOD:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R3);
            anvil_strbuf_append(&be->code, "         LGR   R2,R3\n");
            anvil_strbuf_append(&be->code, "         SRAG  R2,R2,63          Sign extend\n");
            zarch_emit_load_value(be, instr->operands[1], ZARCH_R0);
            anvil_strbuf_append(&be->code, "         DSGR  R2,R0             Divide\n");
            anvil_strbuf_append(&be->code, "         LGR   R15,R2            Remainder to R15\n");
            break;
            
        case ANVIL_OP_AND:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            zarch_emit_load_value(be, instr->operands[1], ZARCH_R3);
            /* NGR - AND 64-bit */
            anvil_strbuf_append(&be->code, "         NGR   R2,R3             AND 64-bit registers\n");
            anvil_strbuf_append(&be->code, "         LGR   R15,R2\n");
            break;
            
        case ANVIL_OP_OR:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            zarch_emit_load_value(be, instr->operands[1], ZARCH_R3);
            /* OGR - OR 64-bit */
            anvil_strbuf_append(&be->code, "         OGR   R2,R3             OR 64-bit registers\n");
            anvil_strbuf_append(&be->code, "         LGR   R15,R2\n");
            break;
            
        case ANVIL_OP_XOR:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            zarch_emit_load_value(be, instr->operands[1], ZARCH_R3);
            /* XGR - XOR 64-bit */
            anvil_strbuf_append(&be->code, "         XGR   R2,R3             XOR 64-bit registers\n");
            anvil_strbuf_append(&be->code, "         LGR   R15,R2\n");
            break;
            
        case ANVIL_OP_SHL:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            zarch_emit_load_value(be, instr->operands[1], ZARCH_R3);
            /* SLLG - Shift Left Logical 64-bit */
            anvil_strbuf_append(&be->code, "         SLLG  R2,R2,0(R3)       Shift left logical 64-bit\n");
            anvil_strbuf_append(&be->code, "         LGR   R15,R2\n");
            break;
            
        case ANVIL_OP_SHR:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            zarch_emit_load_value(be, instr->operands[1], ZARCH_R3);
            /* SRLG - Shift Right Logical 64-bit */
            anvil_strbuf_append(&be->code, "         SRLG  R2,R2,0(R3)       Shift right logical 64-bit\n");
            anvil_strbuf_append(&be->code, "         LGR   R15,R2\n");
            break;
            
        case ANVIL_OP_SAR:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            zarch_emit_load_value(be, instr->operands[1], ZARCH_R3);
            /* SRAG - Shift Right Arithmetic 64-bit */
            anvil_strbuf_append(&be->code, "         SRAG  R2,R2,0(R3)       Shift right arithmetic 64-bit\n");
            anvil_strbuf_append(&be->code, "         LGR   R15,R2\n");
            break;
            
        case ANVIL_OP_NEG:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            /* LCGR - Load Complement 64-bit */
            anvil_strbuf_append(&be->code, "         LCGR  R15,R2            Load complement 64-bit\n");
            break;
            
        case ANVIL_OP_NOT:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            anvil_strbuf_append(&be->code, "         LGHI  R3,-1             Load all 1s\n");
            anvil_strbuf_append(&be->code, "         XGR   R2,R3             XOR with all 1s\n");
            anvil_strbuf_append(&be->code, "         LGR   R15,R2\n");
            break;
            
        case ANVIL_OP_LOAD:
            /* Check if loading from an ALLOCA (stack slot) */
            if (instr->operands[0]->kind == ANVIL_VAL_INSTR &&
                instr->operands[0]->data.instr &&
                instr->operands[0]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = zarch_get_stack_slot(be, instr->operands[0]);
                if (offset >= 0) {
                    anvil_strbuf_appendf(&be->code, "         LG    R15,%d(,R13)       Load from stack slot\n", offset);
                    break;
                }
            }
            /* Check if loading from a global variable */
            if (instr->operands[0]->kind == ANVIL_VAL_GLOBAL) {
                char upper_name[64];
                zarch_uppercase(upper_name, instr->operands[0]->name, sizeof(upper_name));
                /* Use LGRL - Load Relative Long for 64-bit */
                anvil_strbuf_appendf(&be->code, "         LGRL  R15,%s            Load from global\n", upper_name);
                break;
            }
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            anvil_strbuf_append(&be->code, "         LG    R15,0(,R2)        Load 64-bit from address\n");
            break;
            
        case ANVIL_OP_STORE:
            /* Check if storing to an ALLOCA (stack slot) */
            if (instr->operands[1]->kind == ANVIL_VAL_INSTR &&
                instr->operands[1]->data.instr &&
                instr->operands[1]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = zarch_get_stack_slot(be, instr->operands[1]);
                if (offset >= 0) {
                    zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
                    anvil_strbuf_appendf(&be->code, "         STG   R2,%d(,R13)        Store to stack slot\n", offset);
                    break;
                }
            }
            /* Check if storing to a global variable */
            if (instr->operands[1]->kind == ANVIL_VAL_GLOBAL) {
                char upper_name[64];
                zarch_uppercase(upper_name, instr->operands[1]->name, sizeof(upper_name));
                zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
                /* Use STGRL - Store Relative Long for 64-bit globals */
                anvil_strbuf_appendf(&be->code, "         STGRL R2,%s            Store to global\n", upper_name);
                break;
            }
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            zarch_emit_load_value(be, instr->operands[1], ZARCH_R3);
            anvil_strbuf_append(&be->code, "         STG   R2,0(,R3)         Store 64-bit to address\n");
            break;
            
        case ANVIL_OP_STRUCT_GEP:
            /* Get Struct Field Pointer - compute address of struct field (64-bit) */
            {
                zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
                
                int offset = 0;
                if (instr->aux_type && instr->aux_type->kind == ANVIL_TYPE_STRUCT &&
                    instr->num_operands > 1 && instr->operands[1]->kind == ANVIL_VAL_CONST_INT) {
                    unsigned field_idx = (unsigned)instr->operands[1]->data.i;
                    if (field_idx < instr->aux_type->data.struc.num_fields) {
                        offset = (int)instr->aux_type->data.struc.offsets[field_idx];
                    }
                }
                
                if (offset == 0) {
                    anvil_strbuf_append(&be->code, "         LGR   R15,R2             Struct field at offset 0\n");
                } else if (offset > 0 && offset < 4096) {
                    anvil_strbuf_appendf(&be->code, "         LA    R15,%d(,R2)        Struct field at offset %d\n", offset, offset);
                } else {
                    anvil_strbuf_appendf(&be->code, "         LGR   R15,R2             Load base\n");
                    anvil_strbuf_appendf(&be->code, "         AGHI  R15,%d             Add field offset\n", offset);
                }
            }
            break;
            
        case ANVIL_OP_GEP:
            /* Get Element Pointer - compute address of array element (64-bit) */
            {
                zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
                
                if (instr->num_operands > 1) {
                    zarch_emit_load_value(be, instr->operands[1], ZARCH_R3);
                    
                    int elem_size = 8; /* Default to 8 bytes for 64-bit */
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
                                case ANVIL_TYPE_I64:
                                case ANVIL_TYPE_U64:
                                case ANVIL_TYPE_F64:
                                case ANVIL_TYPE_PTR:
                                    elem_size = 8;
                                    break;
                                default:
                                    elem_size = 8;
                                    break;
                            }
                        }
                    }
                    
                    /* Use SLLG for 64-bit shift */
                    if (elem_size == 2) {
                        anvil_strbuf_append(&be->code, "         SLLG  R3,R3,1            Index * 2\n");
                    } else if (elem_size == 4) {
                        anvil_strbuf_append(&be->code, "         SLLG  R3,R3,2            Index * 4\n");
                    } else if (elem_size == 8) {
                        anvil_strbuf_append(&be->code, "         SLLG  R3,R3,3            Index * 8\n");
                    } else if (elem_size != 1) {
                        anvil_strbuf_appendf(&be->code, "         MSGFI R3,%d             Index * %d\n", elem_size, elem_size);
                    }
                    
                    /* AGR - Add 64-bit registers */
                    anvil_strbuf_append(&be->code, "         AGR   R2,R3              Base + offset\n");
                }
                
                anvil_strbuf_append(&be->code, "         LGR   R15,R2             Result pointer\n");
            }
            break;
            
        case ANVIL_OP_BR:
            {
                char upper_func[64], upper_block[64];
                zarch_uppercase(upper_func, be->current_func, sizeof(upper_func));
                zarch_uppercase(upper_block, instr->true_block->name, sizeof(upper_block));
                anvil_strbuf_appendf(&be->code, "         J     %s$%s            Branch relative\n",
                    upper_func, upper_block);
            }
            break;
            
        case ANVIL_OP_BR_COND:
            {
                char upper_func[64], upper_true[64], upper_false[64];
                zarch_uppercase(upper_func, be->current_func, sizeof(upper_func));
                zarch_uppercase(upper_true, instr->true_block->name, sizeof(upper_true));
                zarch_uppercase(upper_false, instr->false_block->name, sizeof(upper_false));
                zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
                anvil_strbuf_append(&be->code, "         LTGR  R2,R2             Test 64-bit register\n");
                anvil_strbuf_appendf(&be->code, "         JNZ   %s$%s            Branch if not zero\n",
                    upper_func, upper_true);
                anvil_strbuf_appendf(&be->code, "         J     %s$%s            Branch to else\n",
                    upper_func, upper_false);
            }
            break;
            
        case ANVIL_OP_RET:
            if (instr->num_operands > 0) {
                zarch_emit_load_value(be, instr->operands[0], ZARCH_R15);
            } else {
                anvil_strbuf_append(&be->code, "         SGR   R15,R15           Return 0\n");
            }
            zarch_emit_epilogue(be);
            break;
            
        case ANVIL_OP_CALL:
            {
                int num_args = (int)(instr->num_operands - 1);
                if (num_args > be->max_call_args) {
                    be->max_call_args = num_args;
                }
                
                /* 64-bit: param list at offset 144 + locals, 8 bytes per param */
                int parm_base = DYN_LOCALS_OFFSET + be->local_vars_size;
                
                anvil_strbuf_append(&be->code, "*        Call setup (reentrant, 64-bit)\n");
                for (size_t i = 1; i < instr->num_operands; i++) {
                    zarch_emit_load_value(be, instr->operands[i], ZARCH_R0);
                    int parm_offset = parm_base + ((int)(i - 1) * 8);
                    anvil_strbuf_appendf(&be->code, "         STG   R0,%d(,R13)       Store param %zu\n", 
                        parm_offset, i - 1);
                }
                
                if (num_args > 0) {
                    anvil_strbuf_appendf(&be->code, "         LA    R1,%d(,R13)       R1 -> param list\n", parm_base);
                    int last_parm_offset = parm_base + ((num_args - 1) * 8);
                    anvil_strbuf_appendf(&be->code, "         OI    %d(R13),X'80'     Mark last param (VL)\n", 
                        last_parm_offset);
                }
                
                /* Use BRASL for 64-bit relative call (uppercase function name) */
                {
                    char upper_callee[64];
                    zarch_uppercase(upper_callee, instr->operands[0]->name, sizeof(upper_callee));
                    anvil_strbuf_appendf(&be->code, "         BRASL R14,%s           Branch relative and save\n", upper_callee);
                }
                
                if (num_args > 0) {
                    int last_parm_offset = parm_base + ((num_args - 1) * 8);
                    anvil_strbuf_appendf(&be->code, "         NI    %d(R13),X'7F'     Clear VL bit\n", 
                        last_parm_offset);
                }
            }
            break;
            
        case ANVIL_OP_CMP_EQ:
        case ANVIL_OP_CMP_NE:
        case ANVIL_OP_CMP_LT:
        case ANVIL_OP_CMP_LE:
        case ANVIL_OP_CMP_GT:
        case ANVIL_OP_CMP_GE:
            {
                const char *branch_cond;
                switch (instr->op) {
                    case ANVIL_OP_CMP_EQ: branch_cond = "JE"; break;
                    case ANVIL_OP_CMP_NE: branch_cond = "JNE"; break;
                    case ANVIL_OP_CMP_LT: branch_cond = "JL"; break;
                    case ANVIL_OP_CMP_LE: branch_cond = "JNH"; break;
                    case ANVIL_OP_CMP_GT: branch_cond = "JH"; break;
                    case ANVIL_OP_CMP_GE: branch_cond = "JNL"; break;
                    default: branch_cond = "JE"; break;
                }
                
                zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
                zarch_emit_load_value(be, instr->operands[1], ZARCH_R3);
                /* CGR - Compare 64-bit registers */
                anvil_strbuf_append(&be->code, "         CGR   R2,R3             Compare 64-bit registers\n");
                anvil_strbuf_append(&be->code, "         LGHI  R15,1             Assume true\n");
                /* z/Arch relative branch Jxx is 4 bytes, SGR is 4 bytes = 8 total */
                anvil_strbuf_appendf(&be->code, "         %-5s *+8               Skip if condition met\n", branch_cond);
                anvil_strbuf_append(&be->code, "         SGR   R15,R15           Set false\n");
            }
            break;
            
        case ANVIL_OP_ZEXT:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            /* LLGFR - Load Logical 64-bit from 32-bit register */
            anvil_strbuf_append(&be->code, "         LLGFR R15,R2            Zero extend to 64-bit\n");
            break;
            
        case ANVIL_OP_SEXT:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            /* LGFR - Load 64-bit from 32-bit with sign extension */
            anvil_strbuf_append(&be->code, "         LGFR  R15,R2            Sign extend to 64-bit\n");
            break;
            
        case ANVIL_OP_TRUNC:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            /* Just copy - high bits will be ignored */
            anvil_strbuf_append(&be->code, "         LGR   R15,R2            Truncate (copy low bits)\n");
            break;
            
        /* ================================================================
         * Floating-Point Operations (HFP or IEEE 754 based on context)
         * z/Architecture has 16 FPRs and supports both formats
         * ================================================================ */
        
        case ANVIL_OP_FADD:
            zarch_emit_load_fp_value(be, instr->operands[0], ZARCH_F0);
            zarch_emit_load_fp_value(be, instr->operands[1], ZARCH_F2);
            {
                bool is_short = (instr->result && instr->result->type && 
                                 instr->result->type->kind == ANVIL_TYPE_F32);
                bool use_ieee = (be->ctx && be->ctx->fp_format == ANVIL_FP_IEEE754);
                if (is_short) {
                    if (use_ieee) {
                        anvil_strbuf_append(&be->code, "         AEBR  0,2               Add short BFP (IEEE)\n");
                    } else {
                        anvil_strbuf_append(&be->code, "         AER   0,2               Add short HFP\n");
                    }
                } else {
                    if (use_ieee) {
                        anvil_strbuf_append(&be->code, "         ADBR  0,2               Add long BFP (IEEE)\n");
                    } else {
                        anvil_strbuf_append(&be->code, "         ADR   0,2               Add long HFP\n");
                    }
                }
            }
            break;
            
        case ANVIL_OP_FSUB:
            zarch_emit_load_fp_value(be, instr->operands[0], ZARCH_F0);
            zarch_emit_load_fp_value(be, instr->operands[1], ZARCH_F2);
            {
                bool is_short = (instr->result && instr->result->type && 
                                 instr->result->type->kind == ANVIL_TYPE_F32);
                bool use_ieee = (be->ctx && be->ctx->fp_format == ANVIL_FP_IEEE754);
                if (is_short) {
                    if (use_ieee) {
                        anvil_strbuf_append(&be->code, "         SEBR  0,2               Sub short BFP (IEEE)\n");
                    } else {
                        anvil_strbuf_append(&be->code, "         SER   0,2               Sub short HFP\n");
                    }
                } else {
                    if (use_ieee) {
                        anvil_strbuf_append(&be->code, "         SDBR  0,2               Sub long BFP (IEEE)\n");
                    } else {
                        anvil_strbuf_append(&be->code, "         SDR   0,2               Sub long HFP\n");
                    }
                }
            }
            break;
            
        case ANVIL_OP_FMUL:
            zarch_emit_load_fp_value(be, instr->operands[0], ZARCH_F0);
            zarch_emit_load_fp_value(be, instr->operands[1], ZARCH_F2);
            {
                bool is_short = (instr->result && instr->result->type && 
                                 instr->result->type->kind == ANVIL_TYPE_F32);
                bool use_ieee = (be->ctx && be->ctx->fp_format == ANVIL_FP_IEEE754);
                if (is_short) {
                    if (use_ieee) {
                        anvil_strbuf_append(&be->code, "         MEEBR 0,2               Mul short BFP (IEEE)\n");
                    } else {
                        anvil_strbuf_append(&be->code, "         MER   0,2               Mul short HFP\n");
                    }
                } else {
                    if (use_ieee) {
                        anvil_strbuf_append(&be->code, "         MDBR  0,2               Mul long BFP (IEEE)\n");
                    } else {
                        anvil_strbuf_append(&be->code, "         MDR   0,2               Mul long HFP\n");
                    }
                }
            }
            break;
            
        case ANVIL_OP_FDIV:
            zarch_emit_load_fp_value(be, instr->operands[0], ZARCH_F0);
            zarch_emit_load_fp_value(be, instr->operands[1], ZARCH_F2);
            {
                bool is_short = (instr->result && instr->result->type && 
                                 instr->result->type->kind == ANVIL_TYPE_F32);
                bool use_ieee = (be->ctx && be->ctx->fp_format == ANVIL_FP_IEEE754);
                if (is_short) {
                    if (use_ieee) {
                        anvil_strbuf_append(&be->code, "         DEBR  0,2               Div short BFP (IEEE)\n");
                    } else {
                        anvil_strbuf_append(&be->code, "         DER   0,2               Div short HFP\n");
                    }
                } else {
                    if (use_ieee) {
                        anvil_strbuf_append(&be->code, "         DDBR  0,2               Div long BFP (IEEE)\n");
                    } else {
                        anvil_strbuf_append(&be->code, "         DDR   0,2               Div long HFP\n");
                    }
                }
            }
            break;
            
        case ANVIL_OP_FNEG:
            zarch_emit_load_fp_value(be, instr->operands[0], ZARCH_F0);
            {
                bool is_short = (instr->result && instr->result->type && 
                                 instr->result->type->kind == ANVIL_TYPE_F32);
                bool use_ieee = (be->ctx && be->ctx->fp_format == ANVIL_FP_IEEE754);
                if (is_short) {
                    if (use_ieee) {
                        anvil_strbuf_append(&be->code, "         LCEBR 0,0               Negate short BFP (IEEE)\n");
                    } else {
                        anvil_strbuf_append(&be->code, "         LCER  0,0               Negate short HFP\n");
                    }
                } else {
                    if (use_ieee) {
                        anvil_strbuf_append(&be->code, "         LCDBR 0,0               Negate long BFP (IEEE)\n");
                    } else {
                        anvil_strbuf_append(&be->code, "         LCDR  0,0               Negate long HFP\n");
                    }
                }
            }
            break;
            
        case ANVIL_OP_FABS:
            zarch_emit_load_fp_value(be, instr->operands[0], ZARCH_F0);
            {
                bool is_short = (instr->result && instr->result->type && 
                                 instr->result->type->kind == ANVIL_TYPE_F32);
                bool use_ieee = (be->ctx && be->ctx->fp_format == ANVIL_FP_IEEE754);
                if (is_short) {
                    if (use_ieee) {
                        anvil_strbuf_append(&be->code, "         LPEBR 0,0               Abs short BFP (IEEE)\n");
                    } else {
                        anvil_strbuf_append(&be->code, "         LPER  0,0               Abs short HFP\n");
                    }
                } else {
                    if (use_ieee) {
                        anvil_strbuf_append(&be->code, "         LPDBR 0,0               Abs long BFP (IEEE)\n");
                    } else {
                        anvil_strbuf_append(&be->code, "         LPDR  0,0               Abs long HFP\n");
                    }
                }
            }
            break;
            
        case ANVIL_OP_FCMP:
            zarch_emit_load_fp_value(be, instr->operands[0], ZARCH_F0);
            zarch_emit_load_fp_value(be, instr->operands[1], ZARCH_F2);
            {
                bool is_short = (instr->operands[0]->type && 
                                 instr->operands[0]->type->kind == ANVIL_TYPE_F32);
                bool use_ieee = (be->ctx && be->ctx->fp_format == ANVIL_FP_IEEE754);
                if (is_short) {
                    if (use_ieee) {
                        anvil_strbuf_append(&be->code, "         CEBR  0,2               Compare short BFP (IEEE)\n");
                    } else {
                        anvil_strbuf_append(&be->code, "         CER   0,2               Compare short HFP\n");
                    }
                } else {
                    if (use_ieee) {
                        anvil_strbuf_append(&be->code, "         CDBR  0,2               Compare long BFP (IEEE)\n");
                    } else {
                        anvil_strbuf_append(&be->code, "         CDR   0,2               Compare long HFP\n");
                    }
                }
            }
            anvil_strbuf_append(&be->code, "         LGHI  R15,1             Assume true\n");
            anvil_strbuf_append(&be->code, "         JE    *+8               Skip if equal\n");
            anvil_strbuf_append(&be->code, "         SGR   R15,R15           Set false\n");
            break;
            
        case ANVIL_OP_SITOFP:
            zarch_emit_load_value(be, instr->operands[0], ZARCH_R2);
            /* z/Architecture has CDFBR for direct int-to-IEEE conversion */
            if (be->ctx && be->ctx->fp_format == ANVIL_FP_IEEE754) {
                if (instr->result && instr->result->type && 
                    instr->result->type->kind == ANVIL_TYPE_F32) {
                    anvil_strbuf_append(&be->code, "         CEFBR 0,R2             Convert int to IEEE short\n");
                } else {
                    anvil_strbuf_append(&be->code, "         CDFBR 0,R2             Convert int to IEEE long\n");
                }
            } else {
                /* HFP conversion */
                anvil_strbuf_append(&be->code, "         STG   R2,144(,R13)      Store int to temp\n");
                anvil_strbuf_append(&be->code, "         SDR   0,0               Clear F0\n");
                anvil_strbuf_append(&be->code, "         LD    0,=D'0'           Load zero\n");
                anvil_strbuf_append(&be->code, "         AW    0,148(,R13)       Add unnormalized word\n");
            }
            break;
            
        case ANVIL_OP_FPTOSI:
            zarch_emit_load_fp_value(be, instr->operands[0], ZARCH_F0);
            /* z/Architecture has CFDBR for direct IEEE-to-int conversion */
            if (be->ctx && be->ctx->fp_format == ANVIL_FP_IEEE754) {
                anvil_strbuf_append(&be->code, "         CFDBR R15,0,0           Convert IEEE long to int\n");
            } else {
                /* HFP conversion using Magic Number technique */
                anvil_strbuf_append(&be->code, "         AW    0,=X'4E00000000000000' Add magic number\n");
                anvil_strbuf_append(&be->code, "         STD   0,152(,R13)       Store result to temp\n");
                anvil_strbuf_append(&be->code, "         L     R15,156(,R13)     Load integer from low word\n");
            }
            break;
            
        case ANVIL_OP_FPEXT:
            zarch_emit_load_fp_value(be, instr->operands[0], ZARCH_F0);
            anvil_strbuf_append(&be->code, "         SDR   2,2               Clear F2\n");
            anvil_strbuf_append(&be->code, "         LER   2,0               Copy short to F2\n");
            anvil_strbuf_append(&be->code, "         LDR   0,2               F0 now has long FP\n");
            break;
            
        case ANVIL_OP_FPTRUNC:
            zarch_emit_load_fp_value(be, instr->operands[0], ZARCH_F0);
            anvil_strbuf_append(&be->code, "         LRER  0,0               Round long to short\n");
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "*        Unimplemented op %d\n", instr->op);
            break;
    }
}

static void zarch_emit_block(zarch_backend_t *be, anvil_block_t *block)
{
    if (!block) return;
    
    /* Block label - use unique label with function name prefix to avoid duplicates */
    char upper_func[64], upper_block[64];
    zarch_uppercase(upper_func, be->current_func, sizeof(upper_func));
    zarch_uppercase(upper_block, block->name, sizeof(upper_block));
    anvil_strbuf_appendf(&be->code, "%s$%s DS    0H\n", upper_func, upper_block);
    
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        zarch_emit_instr(be, instr);
    }
}

static void zarch_emit_func_dynsize(zarch_backend_t *be, anvil_func_t *func)
{
    /* Calculate total stack frame size:
     * 144 (Save Area) + 16 (FP temps) + local_vars + (max_call_args * 8)
     * Align to 16 bytes for 64-bit */
    int total_size = DYN_LOCALS_OFFSET + be->local_vars_size + (be->max_call_args * 8);
    if (total_size % 16 != 0) {
        total_size += (16 - (total_size % 16));
    }
    if (total_size < DYN_LOCALS_OFFSET) total_size = DYN_LOCALS_OFFSET;
    
    char upper_name[64];
    zarch_uppercase(upper_name, func->name, sizeof(upper_name));
    anvil_strbuf_appendf(&be->code, "DYN@%-4s EQU   %d                 Stack frame size for %s\n", 
        upper_name, total_size, upper_name);
}

static void zarch_emit_func(zarch_backend_t *be, anvil_func_t *func)
{
    if (!func) return;
    
    be->local_vars_size = 0;
    be->max_call_args = 0;
    be->num_stack_slots = 0;  /* Reset stack slots for new function */
    
    zarch_emit_prologue(be, func);
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        zarch_emit_block(be, block);
    }
    
    func->stack_size = SA64_SIZE + be->local_vars_size + (be->max_call_args * 8);
    
    /* Drop base register - good practice before next function */
    anvil_strbuf_append(&be->code, "*\n");
    anvil_strbuf_append(&be->code, "         DROP  R12\n");
    anvil_strbuf_append(&be->code, "*\n");
}

static void zarch_emit_footer(zarch_backend_t *be, const char *entry_point)
{
    anvil_strbuf_append(&be->code, "*\n");
    anvil_strbuf_append(&be->code, "         LTORG                    Literal pool\n");
    
    anvil_strbuf_append(&be->code, "*\n");
    anvil_strbuf_append(&be->code, "*        Register equates\n");
    for (int i = 0; i < 16; i++) {
        anvil_strbuf_appendf(&be->code, "R%-7d EQU   %d\n", i, i);
    }
    
    anvil_strbuf_append(&be->code, "*\n");
    if (entry_point) {
        char upper_entry[64];
        zarch_uppercase(upper_entry, entry_point, sizeof(upper_entry));
        anvil_strbuf_appendf(&be->code, "         END   %s\n", upper_entry);
    } else {
        anvil_strbuf_append(&be->code, "         END\n");
    }
}

static anvil_error_t zarch_codegen_module(anvil_backend_t *be, anvil_module_t *mod,
                                           char **output, size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;
    
    zarch_backend_t *priv = be->priv;
    const char *entry_point = NULL;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    
    /* Reset string table */
    priv->num_strings = 0;
    priv->string_counter = 0;
    
    zarch_emit_header(priv, mod->name);
    
    /* Emit code for all functions (skip declarations) */
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (!func->is_declaration) {
            zarch_emit_func(priv, func);
            if (!entry_point) entry_point = func->name;
        }
    }
    
    /* Emit dynamic area size equates (skip declarations) */
    anvil_strbuf_append(&priv->code, "*\n");
    anvil_strbuf_append(&priv->code, "*        Dynamic area sizes (for STORAGE OBTAIN/RELEASE)\n");
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (func->is_declaration) continue;
        priv->local_vars_size = 0;
        priv->max_call_args = 0;
        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
                if (instr->op == ANVIL_OP_CALL) {
                    int num_args = (int)(instr->num_operands - 1);
                    if (num_args > priv->max_call_args) {
                        priv->max_call_args = num_args;
                    }
                } else if (instr->op == ANVIL_OP_ALLOCA) {
                    priv->local_vars_size += 8;  /* 8 bytes for 64-bit */
                }
            }
        }
        zarch_emit_func_dynsize(priv, func);
    }
    
    /* Emit global variables (static) */
    if (mod->num_globals > 0) {
        anvil_strbuf_append(&priv->code, "*\n");
        anvil_strbuf_append(&priv->code, "*        Global variables (static)\n");
        for (anvil_global_t *g = mod->globals; g; g = g->next) {
            char upper_name[64];
            zarch_uppercase(upper_name, g->value->name, sizeof(upper_name));
            
            const char *ds_type = "FD";  /* Default: doubleword (8 bytes) for 64-bit */
            anvil_type_t *type = g->value->type;
            
            if (type) {
                switch (type->kind) {
                    case ANVIL_TYPE_I8:
                    case ANVIL_TYPE_U8:
                        ds_type = "C";   /* Character (1 byte) */
                        break;
                    case ANVIL_TYPE_I16:
                    case ANVIL_TYPE_U16:
                        ds_type = "H";   /* Halfword (2 bytes) */
                        break;
                    case ANVIL_TYPE_I32:
                    case ANVIL_TYPE_U32:
                        ds_type = "F";   /* Fullword (4 bytes) */
                        break;
                    case ANVIL_TYPE_I64:
                    case ANVIL_TYPE_U64:
                    case ANVIL_TYPE_PTR:
                        ds_type = "FD";  /* Doubleword (8 bytes) */
                        break;
                    case ANVIL_TYPE_F32:
                        ds_type = "E";   /* Short FP (4 bytes) */
                        break;
                    case ANVIL_TYPE_F64:
                        ds_type = "D";   /* Long FP (8 bytes) */
                        break;
                    default:
                        ds_type = "FD";
                        break;
                }
            }
            
            if (g->value->data.global.init) {
                anvil_value_t *init = g->value->data.global.init;
                if (init->kind == ANVIL_VAL_CONST_INT) {
                    anvil_strbuf_appendf(&priv->code, "%-8s DC    %s'%lld'            Global variable (initialized)\n",
                        upper_name, ds_type, (long long)init->data.i);
                } else if (init->kind == ANVIL_VAL_CONST_FLOAT) {
                    anvil_strbuf_appendf(&priv->code, "%-8s DC    %s'%g'             Global variable (initialized)\n",
                        upper_name, ds_type, init->data.f);
                } else {
                    anvil_strbuf_appendf(&priv->code, "%-8s DS    %s                  Global variable\n",
                        upper_name, ds_type);
                }
            } else {
                anvil_strbuf_appendf(&priv->code, "%-8s DS    %s                  Global variable\n",
                    upper_name, ds_type);
            }
        }
    }
    
    /* Emit string constants */
    if (priv->num_strings > 0) {
        anvil_strbuf_append(&priv->code, "*\n");
        anvil_strbuf_append(&priv->code, "*        String constants\n");
        for (size_t i = 0; i < priv->num_strings; i++) {
            zarch_string_entry_t *entry = &priv->strings[i];
            /* Emit string - handle special characters by breaking into segments */
            anvil_strbuf_appendf(&priv->code, "%-8s DC    ", entry->label);
            bool in_string = false;
            bool first_segment = true;
            for (const char *p = entry->str; *p; p++) {
                if (*p == '\n') {
                    /* Newline - emit as X'15' (EBCDIC NL) */
                    if (in_string) {
                        anvil_strbuf_append(&priv->code, "'");
                        in_string = false;
                    }
                    if (!first_segment) anvil_strbuf_append(&priv->code, ",");
                    anvil_strbuf_append(&priv->code, "X'15'");
                    first_segment = false;
                } else if (*p == '\r') {
                    /* Carriage return - emit as X'0D' */
                    if (in_string) {
                        anvil_strbuf_append(&priv->code, "'");
                        in_string = false;
                    }
                    if (!first_segment) anvil_strbuf_append(&priv->code, ",");
                    anvil_strbuf_append(&priv->code, "X'0D'");
                    first_segment = false;
                } else if (*p == '\t') {
                    /* Tab - emit as X'05' (EBCDIC HT) */
                    if (in_string) {
                        anvil_strbuf_append(&priv->code, "'");
                        in_string = false;
                    }
                    if (!first_segment) anvil_strbuf_append(&priv->code, ",");
                    anvil_strbuf_append(&priv->code, "X'05'");
                    first_segment = false;
                } else {
                    /* Regular character */
                    if (!in_string) {
                        if (!first_segment) anvil_strbuf_append(&priv->code, ",");
                        anvil_strbuf_append(&priv->code, "C'");
                        in_string = true;
                        first_segment = false;
                    }
                    if (*p == '\'') {
                        anvil_strbuf_append(&priv->code, "''");
                    } else if (*p == '&') {
                        anvil_strbuf_append(&priv->code, "&&");
                    } else {
                        anvil_strbuf_append_char(&priv->code, *p);
                    }
                }
            }
            if (in_string) {
                anvil_strbuf_append(&priv->code, "'");
            }
            /* Add null terminator for C compatibility */
            if (!first_segment) anvil_strbuf_append(&priv->code, ",");
            anvil_strbuf_appendf(&priv->code, "X'00'\n");
        }
    }
    
    zarch_emit_footer(priv, entry_point);
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

static anvil_error_t zarch_codegen_func(anvil_backend_t *be, anvil_func_t *func,
                                         char **output, size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    
    zarch_backend_t *priv = be->priv;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);
    
    zarch_emit_func(priv, func);
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

const anvil_backend_ops_t anvil_backend_zarch = {
    .name = "z/Architecture",
    .arch = ANVIL_ARCH_ZARCH,
    .init = zarch_init,
    .cleanup = zarch_cleanup,
    .reset = zarch_reset,
    .codegen_module = zarch_codegen_module,
    .codegen_func = zarch_codegen_func,
    .get_arch_info = zarch_get_arch_info
};
