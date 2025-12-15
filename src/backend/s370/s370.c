/*
 * ANVIL - IBM S/370 Backend
 * 
 * Big-endian, stack grows UPWARD (toward higher addresses)
 * 24-bit addressing mode
 * Generates HLASM (High Level Assembler) syntax
 * 
 * Register conventions (MVS linkage):
 *   R0      - Work register (volatile)
 *   R1      - Parameter list pointer (points to list of addresses)
 *   R2-R11  - General purpose / work registers
 *   R12     - Base register for addressability
 *   R13     - Save area pointer
 *   R14     - Return address
 *   R15     - Entry point address / return code
 * 
 * Save area format (18 fullwords = 72 bytes):
 *   +0   - Reserved (used by PL/I)
 *   +4   - Pointer to previous save area (caller's SA)
 *   +8   - Pointer to next save area (callee's SA)
 *   +12  - R14 (return address)
 *   +16  - R15 (entry point)
 *   +20  - R0
 *   +24  - R1
 *   +28  - R2
 *   ...  - ...
 *   +68  - R12
 * 
 * Parameter passing (MVS standard):
 *   R1 points to a list of fullword addresses
 *   Each address points to the actual parameter value
 *   High-order bit of last address is set to 1
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* S/370 registers */
static const char *s370_reg_names[] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
    "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"
};

/* S/370 floating-point registers (HFP uses pairs: 0,2,4,6) */
static const char *s370_fpr_names[] = {
    "F0", "F2", "F4", "F6"
};

/* FPR indices (actual register numbers) */
#define S370_F0  0
#define S370_F2  2
#define S370_F4  4
#define S370_F6  6

/* Register usage */
#define S370_R0   0   /* Work register */
#define S370_R1   1   /* Parameter pointer */
#define S370_R2   2   /* First param / work */
#define S370_R3   3   /* Second param / work */
#define S370_R4   4   /* Work register */
#define S370_R5   5   /* Work register */
#define S370_R12  12  /* Base register */
#define S370_R13  13  /* Save area pointer */
#define S370_R14  14  /* Return address */
#define S370_R15  15  /* Entry point / return code */

/* Save area offsets */
#define SA_PREV   4
#define SA_NEXT   8
#define SA_R14    12
#define SA_R15    16
#define SA_R0     20
#define SA_SIZE   72

/* Dynamic storage layout (relative to R13):
 * +0   - Save Area (72 bytes)
 * +72  - FP temp area (8 bytes for double)
 * +80  - FP temp area 2 (8 bytes for conversions)
 * +88  - Local variables start
 * +N   - Parameter list for outgoing calls
 */
#define FP_TEMP_OFFSET     72   /* FP temporary storage */
#define FP_TEMP2_OFFSET    80   /* FP temporary storage 2 (for conversions) */
#define DYN_LOCALS_OFFSET  88   /* Locals start after save area + FP temps */

/* String table entry */
typedef struct {
    const char *str;
    char label[16];
    size_t len;
} s370_string_entry_t;

/* Stack slot for local variables */
typedef struct {
    anvil_value_t *value;     /* The ALLOCA result value */
    int offset;               /* Offset from R13 */
} s370_stack_slot_t;

/* Backend private data */
typedef struct {
    anvil_strbuf_t code;
    anvil_strbuf_t data;
    int label_counter;
    int literal_counter;
    int string_counter;
    int local_vars_size;      /* Size of local variables */
    int max_call_args;        /* Max args in any CALL (for param list) */
    int parm_list_offset;     /* Offset to param list in dynamic area */
    const char *csect_name;
    const char *current_func; /* Current function name for DYNSIZE */
    
    /* Stack slots for local variables */
    s370_stack_slot_t *stack_slots;
    size_t num_stack_slots;
    size_t stack_slots_cap;
    
    /* String table */
    s370_string_entry_t *strings;
    size_t num_strings;
    size_t strings_cap;
} s370_backend_t;

static const anvil_arch_info_t s370_arch_info = {
    .arch = ANVIL_ARCH_S370,
    .name = "S/370",
    .ptr_size = 4,
    .addr_bits = 24,
    .word_size = 4,
    .num_gpr = 16,
    .num_fpr = 4,
    .endian = ANVIL_ENDIAN_BIG,
    .stack_dir = ANVIL_STACK_UP,
    .has_condition_codes = true,
    .has_delay_slots = false
};

static anvil_error_t s370_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    (void)ctx;
    s370_backend_t *priv = calloc(1, sizeof(s370_backend_t));
    if (!priv) return ANVIL_ERR_NOMEM;
    
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->csect_name = "CODE";
    
    be->priv = priv;
    return ANVIL_OK;
}

static void s370_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    s370_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv->strings);
    free(priv->stack_slots);
    free(priv);
    be->priv = NULL;
}

/* Get stack slot offset for an ALLOCA result value */
static int s370_get_stack_slot(s370_backend_t *be, anvil_value_t *val)
{
    for (size_t i = 0; i < be->num_stack_slots; i++) {
        if (be->stack_slots[i].value == val) {
            return be->stack_slots[i].offset;
        }
    }
    return -1;  /* Not found */
}

/* Add a stack slot for an ALLOCA */
static int s370_add_stack_slot(s370_backend_t *be, anvil_value_t *val)
{
    if (be->num_stack_slots >= be->stack_slots_cap) {
        size_t new_cap = be->stack_slots_cap ? be->stack_slots_cap * 2 : 16;
        s370_stack_slot_t *new_slots = realloc(be->stack_slots, new_cap * sizeof(s370_stack_slot_t));
        if (!new_slots) return -1;
        be->stack_slots = new_slots;
        be->stack_slots_cap = new_cap;
    }
    
    int offset = DYN_LOCALS_OFFSET + be->local_vars_size;
    be->stack_slots[be->num_stack_slots].value = val;
    be->stack_slots[be->num_stack_slots].offset = offset;
    be->num_stack_slots++;
    be->local_vars_size += 4;  /* 4 bytes per slot */
    
    return offset;
}

static const anvil_arch_info_t *s370_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &s370_arch_info;
}

/* Convert function name to uppercase (GCCMVS convention) */
static void s370_uppercase(char *dest, const char *src, size_t max_len)
{
    size_t i;
    for (i = 0; i < max_len - 1 && src[i]; i++) {
        dest[i] = (src[i] >= 'a' && src[i] <= 'z') ? src[i] - 32 : src[i];
    }
    dest[i] = '\0';
}

/* Emit HLASM header - GCCMVS compatible */
static void s370_emit_header(s370_backend_t *be, const char *module_name)
{
    (void)module_name;
    anvil_strbuf_append(&be->code, "***********************************************************************\n");
    anvil_strbuf_appendf(&be->code, "*        Generated by ANVIL for IBM S/370\n");
    anvil_strbuf_append(&be->code, "***********************************************************************\n");
    anvil_strbuf_append(&be->code, "         CSECT\n");
    anvil_strbuf_append(&be->code, "         AMODE ANY\n");
    anvil_strbuf_append(&be->code, "         RMODE ANY\n");
    anvil_strbuf_append(&be->code, "*\n");
}

/* Emit function prologue - GCCMVS style (stack allocation, no GETMAIN) */
static void s370_emit_prologue(s370_backend_t *be, anvil_func_t *func)
{
    char upper_name[64];
    s370_uppercase(upper_name, func->name, sizeof(upper_name));
    be->current_func = func->name;
    
    /* Entry point label (uppercase) */
    anvil_strbuf_appendf(&be->code, "%-8s DS    0H\n", upper_name);
    
    /* 1. Save caller's registers in CALLER's save area */
    anvil_strbuf_append(&be->code, "         STM   R14,R12,12(R13)    Save caller's registers\n");
    
    /* 2. Establish addressability using R12 as base */
    anvil_strbuf_append(&be->code, "         LR    R12,R15            Copy entry point to base reg\n");
    anvil_strbuf_appendf(&be->code, "         USING %s,R12            Establish addressability\n", upper_name);
    
    /* 3. Save R1 (param pointer) in R11 */
    anvil_strbuf_append(&be->code, "         LR    R11,R1             Save parameter list pointer\n");
    
    /* 4. Set up save area chain using stack (no GETMAIN) */
    anvil_strbuf_append(&be->code, "*        Set up save area chain (stack allocation)\n");
    anvil_strbuf_append(&be->code, "         LA    R2,72(,R13)        R2 -> our save area (after caller's)\n");
    anvil_strbuf_append(&be->code, "         ST    R13,4(,R2)         Chain: new->prev = caller's\n");
    anvil_strbuf_append(&be->code, "         ST    R2,8(,R13)         Chain: caller->next = new\n");
    anvil_strbuf_append(&be->code, "         LR    R13,R2             R13 -> our save area\n");
    anvil_strbuf_append(&be->code, "*\n");
}

/* Emit function epilogue - GCCMVS style (no FREEMAIN) */
static void s370_emit_epilogue(s370_backend_t *be)
{
    (void)be;
    anvil_strbuf_append(&be->code, "*        Function epilogue\n");
    
    /* 1. Restore caller's save area pointer */
    anvil_strbuf_append(&be->code, "         L     R13,4(,R13)        Restore caller's SA pointer\n");
    
    /* 2. Restore registers - R15 has return value, don't overwrite it */
    anvil_strbuf_append(&be->code, "         L     R14,12(,R13)       Restore return address\n");
    anvil_strbuf_append(&be->code, "         LM    R0,R12,20(,R13)    Restore R0-R12\n");
    anvil_strbuf_append(&be->code, "         BR    R14                Return to caller\n");
}

/* Add string to string table and return its label */
static const char *s370_add_string(s370_backend_t *be, const char *str)
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
        s370_string_entry_t *new_strings = realloc(be->strings, 
            new_cap * sizeof(s370_string_entry_t));
        if (!new_strings) return "STR$ERR";
        be->strings = new_strings;
        be->strings_cap = new_cap;
    }
    
    /* Add new string */
    s370_string_entry_t *entry = &be->strings[be->num_strings];
    entry->str = str;
    entry->len = strlen(str);
    snprintf(entry->label, sizeof(entry->label), "STR$%d", be->string_counter++);
    be->num_strings++;
    
    return entry->label;
}

/* Emit floating-point value to FP register (HFP format) */
static void s370_emit_load_fp_value(s370_backend_t *be, anvil_value_t *val, int target_fpr)
{
    if (!val) return;
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_FLOAT:
            /* Load FP constant using literal */
            if (val->type && val->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_appendf(&be->code, "         LE    %d,=E'%g'         Load short FP constant\n",
                    target_fpr, val->data.f);
            } else {
                anvil_strbuf_appendf(&be->code, "         LD    %d,=D'%g'         Load long FP constant\n",
                    target_fpr, val->data.f);
            }
            break;
            
        case ANVIL_VAL_INSTR:
            /* FP result - assume in F0 */
            if (target_fpr != S370_F0) {
                if (val->type && val->type->kind == ANVIL_TYPE_F32) {
                    anvil_strbuf_appendf(&be->code, "         LER   %d,0             Copy short FP result\n", target_fpr);
                } else {
                    anvil_strbuf_appendf(&be->code, "         LDR   %d,0             Copy long FP result\n", target_fpr);
                }
            }
            break;
            
        case ANVIL_VAL_PARAM:
            /* FP parameter - load from parameter area */
            /* MVS passes FP params in memory, pointed to by R1 list */
            anvil_strbuf_appendf(&be->code, "         L     R2,%zu(,R11)       Load addr of FP param %zu\n",
                val->data.param.index * 4, val->data.param.index);
            anvil_strbuf_append(&be->code, "         N     R2,=X'7FFFFFFF'   Clear VL bit\n");
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

/* Emit value to register */
static void s370_emit_load_value(s370_backend_t *be, anvil_value_t *val, int target_reg)
{
    if (!val) return;
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_INT:
            if (val->data.i >= 0 && val->data.i <= 4095) {
                /* Use LA for small positive constants (0-4095) */
                anvil_strbuf_appendf(&be->code, "         LA    %s,%lld            Load constant\n",
                    s370_reg_names[target_reg], (long long)val->data.i);
            } else if (val->data.i >= -32768 && val->data.i <= 32767) {
                /* Use L with halfword literal */
                anvil_strbuf_appendf(&be->code, "         L     %s,=F'%lld'       Load constant\n",
                    s370_reg_names[target_reg], (long long)val->data.i);
            } else {
                /* Use L with fullword literal */
                anvil_strbuf_appendf(&be->code, "         L     %s,=F'%lld'       Load constant\n",
                    s370_reg_names[target_reg], (long long)val->data.i);
            }
            break;
            
        case ANVIL_VAL_CONST_STRING:
            {
                /* Add string to table and load its address */
                const char *label = s370_add_string(be, val->data.str ? val->data.str : "");
                anvil_strbuf_appendf(&be->code, "         LA    %s,%s            Load string address\n",
                    s370_reg_names[target_reg], label);
            }
            break;
            
        case ANVIL_VAL_PARAM:
            /* MVS parameter passing: R11 has saved R1 (param list pointer) */
            /* R1 points to list of ADDRESSES, each address points to the value */
            /* Step 1: Load address of parameter from list */
            anvil_strbuf_appendf(&be->code, "         L     %s,%zu(,R11)       Load addr of param %zu\n",
                s370_reg_names[target_reg], val->data.param.index * 4, val->data.param.index);
            /* Note: We do NOT clear the VL bit - this allows full 31/64-bit addressing */
            /* Step 2: Load actual value from that address */
            anvil_strbuf_appendf(&be->code, "         L     %s,0(,%s)         Load param value\n",
                s370_reg_names[target_reg], s370_reg_names[target_reg]);
            break;
            
        case ANVIL_VAL_INSTR:
            /* Check if this is an ALLOCA result - use stack slot offset (address) */
            if (val->data.instr && val->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = s370_get_stack_slot(be, val);
                if (offset >= 0) {
                    anvil_strbuf_appendf(&be->code, "         LA    %s,%d(,R13)       Load addr of local var\n",
                        s370_reg_names[target_reg], offset);
                    break;
                }
            }
            /* Check if this is a LOAD from a stack slot - load value directly */
            if (val->data.instr && val->data.instr->op == ANVIL_OP_LOAD) {
                anvil_instr_t *load_instr = val->data.instr;
                if (load_instr->operands[0]->kind == ANVIL_VAL_INSTR &&
                    load_instr->operands[0]->data.instr &&
                    load_instr->operands[0]->data.instr->op == ANVIL_OP_ALLOCA) {
                    int offset = s370_get_stack_slot(be, load_instr->operands[0]);
                    if (offset >= 0) {
                        anvil_strbuf_appendf(&be->code, "         L     %s,%d(,R13)       Load value from stack slot\n",
                            s370_reg_names[target_reg], offset);
                        break;
                    }
                }
            }
            /* Otherwise, result in R15 by convention */
            if (target_reg != S370_R15) {
                anvil_strbuf_appendf(&be->code, "         LR    %s,R15            Copy result\n",
                    s370_reg_names[target_reg]);
            }
            break;
            
        case ANVIL_VAL_GLOBAL:
            anvil_strbuf_appendf(&be->code, "         L     %s,%s            Load global\n",
                s370_reg_names[target_reg], val->name);
            break;
            
        case ANVIL_VAL_FUNC:
            /* Load function entry point address */
            anvil_strbuf_appendf(&be->code, "         L     %s,=V(%s)        Load function address\n",
                s370_reg_names[target_reg], val->name);
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "*        Unknown value kind %d\n", val->kind);
            break;
    }
}

static void s370_emit_instr(s370_backend_t *be, anvil_instr_t *instr)
{
    if (!instr) return;
    
    switch (instr->op) {
        case ANVIL_OP_ALLOCA:
            /* Allocate space in dynamic area for local variable */
            /* Register the stack slot for this ALLOCA result */
            {
                int offset = s370_add_stack_slot(be, instr->result);
                /* Initialize to zero using XC (Exclusive Or Character) */
                anvil_strbuf_appendf(&be->code, "         XC    %d(4,R13),%d(R13)  Init local var to 0\n", 
                    offset, offset);
            }
            break;
            
        case ANVIL_OP_ADD:
            s370_emit_load_value(be, instr->operands[0], S370_R2);
            s370_emit_load_value(be, instr->operands[1], S370_R3);
            anvil_strbuf_append(&be->code, "         AR    R2,R3             Add registers\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2            Result in R15\n");
            break;
            
        case ANVIL_OP_SUB:
            s370_emit_load_value(be, instr->operands[0], S370_R2);
            s370_emit_load_value(be, instr->operands[1], S370_R3);
            anvil_strbuf_append(&be->code, "         SR    R2,R3             Subtract registers\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2            Result in R15\n");
            break;
            
        case ANVIL_OP_MUL:
            /* MR uses even-odd register pair (R2,R3)
             * MR R2,Rx multiplies R3 by Rx, result in R2:R3
             * We load multiplicand into R3 (odd), multiplier into R4
             * Then MR R2,R4 computes R3 * R4 -> R2:R3
             */
            s370_emit_load_value(be, instr->operands[0], S370_R3);  /* multiplicand in R3 (odd) */
            s370_emit_load_value(be, instr->operands[1], S370_R4);  /* multiplier in R4 */
            anvil_strbuf_append(&be->code, "         MR    R2,R4             R2:R3 = R3 * R4\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R3            Low 32 bits to R15\n");
            break;
            
        case ANVIL_OP_SDIV:
            /* DR uses even-odd pair, dividend in R2:R3, divisor in operand */
            s370_emit_load_value(be, instr->operands[0], S370_R3);
            anvil_strbuf_append(&be->code, "         SRDA  R2,32             Sign extend R3 into R2:R3\n");
            s370_emit_load_value(be, instr->operands[1], S370_R0);
            anvil_strbuf_append(&be->code, "         DR    R2,R0             Divide R2:R3 by R0\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R3            Quotient to R15\n");
            break;
            
        case ANVIL_OP_SMOD:
            s370_emit_load_value(be, instr->operands[0], S370_R3);
            anvil_strbuf_append(&be->code, "         SRDA  R2,32             Sign extend\n");
            s370_emit_load_value(be, instr->operands[1], S370_R0);
            anvil_strbuf_append(&be->code, "         DR    R2,R0             Divide\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2            Remainder to R15\n");
            break;
            
        case ANVIL_OP_AND:
            s370_emit_load_value(be, instr->operands[0], S370_R2);
            s370_emit_load_value(be, instr->operands[1], S370_R3);
            anvil_strbuf_append(&be->code, "         NR    R2,R3             AND registers\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2\n");
            break;
            
        case ANVIL_OP_OR:
            s370_emit_load_value(be, instr->operands[0], S370_R2);
            s370_emit_load_value(be, instr->operands[1], S370_R3);
            anvil_strbuf_append(&be->code, "         OR    R2,R3             OR registers\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2\n");
            break;
            
        case ANVIL_OP_XOR:
            s370_emit_load_value(be, instr->operands[0], S370_R2);
            s370_emit_load_value(be, instr->operands[1], S370_R3);
            anvil_strbuf_append(&be->code, "         XR    R2,R3             XOR registers\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2\n");
            break;
            
        case ANVIL_OP_SHL:
            s370_emit_load_value(be, instr->operands[0], S370_R2);
            s370_emit_load_value(be, instr->operands[1], S370_R3);
            anvil_strbuf_append(&be->code, "         SLL   R2,0(R3)          Shift left logical\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2\n");
            break;
            
        case ANVIL_OP_SHR:
            s370_emit_load_value(be, instr->operands[0], S370_R2);
            s370_emit_load_value(be, instr->operands[1], S370_R3);
            anvil_strbuf_append(&be->code, "         SRL   R2,0(R3)          Shift right logical\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2\n");
            break;
            
        case ANVIL_OP_SAR:
            s370_emit_load_value(be, instr->operands[0], S370_R2);
            s370_emit_load_value(be, instr->operands[1], S370_R3);
            anvil_strbuf_append(&be->code, "         SRA   R2,0(R3)          Shift right arithmetic\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2\n");
            break;
            
        case ANVIL_OP_NEG:
            s370_emit_load_value(be, instr->operands[0], S370_R2);
            anvil_strbuf_append(&be->code, "         LCR   R15,R2            Load complement\n");
            break;
            
        case ANVIL_OP_NOT:
            s370_emit_load_value(be, instr->operands[0], S370_R2);
            anvil_strbuf_append(&be->code, "         X     R2,=F'-1'         XOR with all 1s\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2\n");
            break;
            
        case ANVIL_OP_LOAD:
            /* Check if loading from an ALLOCA (stack slot) */
            if (instr->operands[0]->kind == ANVIL_VAL_INSTR &&
                instr->operands[0]->data.instr &&
                instr->operands[0]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = s370_get_stack_slot(be, instr->operands[0]);
                if (offset >= 0) {
                    anvil_strbuf_appendf(&be->code, "         L     R15,%d(,R13)       Load from stack slot\n", offset);
                    break;
                }
            }
            /* Generic load from address in register */
            s370_emit_load_value(be, instr->operands[0], S370_R2);
            anvil_strbuf_append(&be->code, "         L     R15,0(,R2)        Load from address\n");
            break;
            
        case ANVIL_OP_STORE:
            /* Check if storing to an ALLOCA (stack slot) */
            if (instr->operands[1]->kind == ANVIL_VAL_INSTR &&
                instr->operands[1]->data.instr &&
                instr->operands[1]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = s370_get_stack_slot(be, instr->operands[1]);
                if (offset >= 0) {
                    s370_emit_load_value(be, instr->operands[0], S370_R2);  /* value */
                    anvil_strbuf_appendf(&be->code, "         ST    R2,%d(,R13)        Store to stack slot\n", offset);
                    break;
                }
            }
            /* Generic store to address in register */
            s370_emit_load_value(be, instr->operands[0], S370_R2);  /* value */
            s370_emit_load_value(be, instr->operands[1], S370_R3);  /* address */
            anvil_strbuf_append(&be->code, "         ST    R2,0(,R3)         Store to address\n");
            break;
            
        case ANVIL_OP_STRUCT_GEP:
            /* Get Struct Field Pointer - compute address of struct field
             * operands[0] = base pointer to struct
             * operands[1] = field index (constant)
             * aux_type = struct type (for offset lookup)
             */
            {
                s370_emit_load_value(be, instr->operands[0], S370_R2);
                
                /* Get field offset from struct type */
                int offset = 0;
                if (instr->aux_type && instr->aux_type->kind == ANVIL_TYPE_STRUCT &&
                    instr->num_operands > 1 && instr->operands[1]->kind == ANVIL_VAL_CONST_INT) {
                    unsigned field_idx = (unsigned)instr->operands[1]->data.i;
                    if (field_idx < instr->aux_type->data.struc.num_fields) {
                        offset = (int)instr->aux_type->data.struc.offsets[field_idx];
                    }
                }
                
                if (offset == 0) {
                    /* No offset needed, just copy base pointer */
                    anvil_strbuf_append(&be->code, "         LR    R15,R2             Struct field at offset 0\n");
                } else if (offset > 0 && offset < 4096) {
                    /* Use LA for small positive offsets */
                    anvil_strbuf_appendf(&be->code, "         LA    R15,%d(,R2)        Struct field at offset %d\n", offset, offset);
                } else {
                    /* Large offset - use AHI or add literal */
                    anvil_strbuf_appendf(&be->code, "         LA    R15,0(,R2)         Load base\n");
                    anvil_strbuf_appendf(&be->code, "         A     R15,=F'%d'         Add field offset %d\n", offset, offset);
                }
            }
            break;
            
        case ANVIL_OP_GEP:
            /* Get Element Pointer - compute address of array element
             * operands[0] = base pointer
             * operands[1..n] = indices
             * 
             * For array access: result = base + (index * element_size)
             * Uses D(X,B) addressing: displacement(index_reg, base_reg)
             */
            {
                /* Load base pointer */
                s370_emit_load_value(be, instr->operands[0], S370_R2);
                
                if (instr->num_operands > 1) {
                    /* Load first index */
                    s370_emit_load_value(be, instr->operands[1], S370_R3);
                    
                    /* Determine element size from result type */
                    int elem_size = 4; /* Default to 4 bytes (int) */
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
                                    elem_size = 8;
                                    break;
                                default:
                                    elem_size = 4;
                                    break;
                            }
                        }
                    }
                    
                    /* Multiply index by element size */
                    if (elem_size == 1) {
                        /* No multiplication needed */
                    } else if (elem_size == 2) {
                        anvil_strbuf_append(&be->code, "         SLL   R3,1               Index * 2\n");
                    } else if (elem_size == 4) {
                        anvil_strbuf_append(&be->code, "         SLL   R3,2               Index * 4\n");
                    } else if (elem_size == 8) {
                        anvil_strbuf_append(&be->code, "         SLL   R3,3               Index * 8\n");
                    } else {
                        /* Generic multiplication */
                        anvil_strbuf_appendf(&be->code, "         MH    R3,=H'%d'          Index * %d\n", elem_size, elem_size);
                    }
                    
                    /* Add index offset to base: result = base + (index * size) */
                    anvil_strbuf_append(&be->code, "         AR    R2,R3              Base + offset\n");
                }
                
                /* Result address in R15 */
                anvil_strbuf_append(&be->code, "         LR    R15,R2             Result pointer\n");
            }
            break;
            
        case ANVIL_OP_BR:
            {
                char upper_func[64], upper_block[64];
                s370_uppercase(upper_func, be->current_func, sizeof(upper_func));
                s370_uppercase(upper_block, instr->true_block->name, sizeof(upper_block));
                anvil_strbuf_appendf(&be->code, "         B     %s$%s            Branch unconditional\n",
                    upper_func, upper_block);
            }
            break;
            
        case ANVIL_OP_BR_COND:
            {
                char upper_func[64], upper_true[64], upper_false[64];
                s370_uppercase(upper_func, be->current_func, sizeof(upper_func));
                s370_uppercase(upper_true, instr->true_block->name, sizeof(upper_true));
                s370_uppercase(upper_false, instr->false_block->name, sizeof(upper_false));
                s370_emit_load_value(be, instr->operands[0], S370_R2);
                anvil_strbuf_append(&be->code, "         LTR   R2,R2             Test register\n");
                anvil_strbuf_appendf(&be->code, "         BNZ   %s$%s            Branch if not zero\n",
                    upper_func, upper_true);
                anvil_strbuf_appendf(&be->code, "         B     %s$%s            Branch to else\n",
                    upper_func, upper_false);
            }
            break;
            
        case ANVIL_OP_RET:
            if (instr->num_operands > 0) {
                s370_emit_load_value(be, instr->operands[0], S370_R15);
            } else {
                anvil_strbuf_append(&be->code, "         SR    R15,R15           Return 0\n");
            }
            s370_emit_epilogue(be);
            break;
            
        case ANVIL_OP_CALL:
            {
                /* Track max args for dynamic area sizing */
                int num_args = (int)(instr->num_operands - 1);
                if (num_args > be->max_call_args) {
                    be->max_call_args = num_args;
                }
                
                /* Build parameter list in dynamic area (relative to R13) */
                /* Param list starts at offset: 72 (SA) + local_vars_size */
                int parm_base = DYN_LOCALS_OFFSET + be->local_vars_size;
                
                anvil_strbuf_append(&be->code, "*        Call setup (reentrant)\n");
                for (size_t i = 1; i < instr->num_operands; i++) {
                    s370_emit_load_value(be, instr->operands[i], S370_R0);
                    int parm_offset = parm_base + ((int)(i - 1) * 4);
                    anvil_strbuf_appendf(&be->code, "         ST    R0,%d(,R13)       Store param %zu\n", 
                        parm_offset, i - 1);
                }
                
                if (num_args > 0) {
                    /* Point R1 to param list in our dynamic area */
                    anvil_strbuf_appendf(&be->code, "         LA    R1,%d(,R13)       R1 -> param list\n", parm_base);
                    /* Set high bit on last parameter address (MVS VL convention) */
                    int last_parm_offset = parm_base + ((num_args - 1) * 4);
                    anvil_strbuf_appendf(&be->code, "         OI    %d(R13),X'80'     Mark last param (VL)\n", 
                        last_parm_offset);
                }
                
                /* Load entry point and call (uppercase function name) */
                {
                    char upper_callee[64];
                    s370_uppercase(upper_callee, instr->operands[0]->name, sizeof(upper_callee));
                    anvil_strbuf_appendf(&be->code, "         L     R15,=V(%s)        Load entry point\n", upper_callee);
                }
                anvil_strbuf_append(&be->code, "         BALR  R14,R15           Call subroutine\n");
                
                /* Clear VL bit after call (in case we reuse the slot) */
                if (num_args > 0) {
                    int last_parm_offset = parm_base + ((num_args - 1) * 4);
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
                    case ANVIL_OP_CMP_EQ: branch_cond = "BE"; break;
                    case ANVIL_OP_CMP_NE: branch_cond = "BNE"; break;
                    case ANVIL_OP_CMP_LT: branch_cond = "BL"; break;
                    case ANVIL_OP_CMP_LE: branch_cond = "BNH"; break;
                    case ANVIL_OP_CMP_GT: branch_cond = "BH"; break;
                    case ANVIL_OP_CMP_GE: branch_cond = "BNL"; break;
                    default: branch_cond = "BE"; break;
                }
                
                s370_emit_load_value(be, instr->operands[0], S370_R2);
                s370_emit_load_value(be, instr->operands[1], S370_R3);
                anvil_strbuf_append(&be->code, "         CR    R2,R3             Compare registers\n");
                anvil_strbuf_append(&be->code, "         LA    R15,1             Assume true\n");
                /* CRITICAL: Branch offset is +6, not +8!
                 * Bxx instruction = 4 bytes, SR = 2 bytes, total = 6 */
                anvil_strbuf_appendf(&be->code, "         %-5s *+6               Skip if condition met\n", branch_cond);
                anvil_strbuf_append(&be->code, "         SR    R15,R15           Set false\n");
            }
            break;
            
        /* ================================================================
         * Floating-Point Operations (HFP - Hexadecimal Floating Point)
         * S/370 uses short (32-bit, E format) and long (64-bit, D format)
         * FP registers: 0, 2, 4, 6 (even numbers only)
         * ================================================================ */
        
        case ANVIL_OP_FADD:
            /* Floating-point add */
            s370_emit_load_fp_value(be, instr->operands[0], S370_F0);
            s370_emit_load_fp_value(be, instr->operands[1], S370_F2);
            if (instr->result && instr->result->type && 
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "         AER   0,2               Add short FP (F0 = F0 + F2)\n");
            } else {
                anvil_strbuf_append(&be->code, "         ADR   0,2               Add long FP (F0 = F0 + F2)\n");
            }
            break;
            
        case ANVIL_OP_FSUB:
            /* Floating-point subtract */
            s370_emit_load_fp_value(be, instr->operands[0], S370_F0);
            s370_emit_load_fp_value(be, instr->operands[1], S370_F2);
            if (instr->result && instr->result->type && 
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "         SER   0,2               Sub short FP (F0 = F0 - F2)\n");
            } else {
                anvil_strbuf_append(&be->code, "         SDR   0,2               Sub long FP (F0 = F0 - F2)\n");
            }
            break;
            
        case ANVIL_OP_FMUL:
            /* Floating-point multiply */
            s370_emit_load_fp_value(be, instr->operands[0], S370_F0);
            s370_emit_load_fp_value(be, instr->operands[1], S370_F2);
            if (instr->result && instr->result->type && 
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "         MER   0,2               Mul short FP (F0 = F0 * F2)\n");
            } else {
                anvil_strbuf_append(&be->code, "         MDR   0,2               Mul long FP (F0 = F0 * F2)\n");
            }
            break;
            
        case ANVIL_OP_FDIV:
            /* Floating-point divide */
            s370_emit_load_fp_value(be, instr->operands[0], S370_F0);
            s370_emit_load_fp_value(be, instr->operands[1], S370_F2);
            if (instr->result && instr->result->type && 
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "         DER   0,2               Div short FP (F0 = F0 / F2)\n");
            } else {
                anvil_strbuf_append(&be->code, "         DDR   0,2               Div long FP (F0 = F0 / F2)\n");
            }
            break;
            
        case ANVIL_OP_FNEG:
            /* Floating-point negate */
            s370_emit_load_fp_value(be, instr->operands[0], S370_F0);
            if (instr->result && instr->result->type && 
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "         LCER  0,0               Negate short FP\n");
            } else {
                anvil_strbuf_append(&be->code, "         LCDR  0,0               Negate long FP\n");
            }
            break;
            
        case ANVIL_OP_FABS:
            /* Floating-point absolute value */
            s370_emit_load_fp_value(be, instr->operands[0], S370_F0);
            if (instr->result && instr->result->type && 
                instr->result->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "         LPER  0,0               Abs short FP\n");
            } else {
                anvil_strbuf_append(&be->code, "         LPDR  0,0               Abs long FP\n");
            }
            break;
            
        case ANVIL_OP_FCMP:
            /* Floating-point compare - sets condition code */
            s370_emit_load_fp_value(be, instr->operands[0], S370_F0);
            s370_emit_load_fp_value(be, instr->operands[1], S370_F2);
            if (instr->operands[0]->type && 
                instr->operands[0]->type->kind == ANVIL_TYPE_F32) {
                anvil_strbuf_append(&be->code, "         CER   0,2               Compare short FP\n");
            } else {
                anvil_strbuf_append(&be->code, "         CDR   0,2               Compare long FP\n");
            }
            /* Result is in condition code, convert to integer 0/1 */
            anvil_strbuf_append(&be->code, "         LA    R15,1             Assume true\n");
            anvil_strbuf_append(&be->code, "         BE    *+6               Skip if equal\n");
            anvil_strbuf_append(&be->code, "         SR    R15,R15           Set false\n");
            break;
            
        case ANVIL_OP_SITOFP:
            /* Signed integer to floating-point */
            s370_emit_load_value(be, instr->operands[0], S370_R2);
            /* Store integer to temp, then load as FP and convert */
            anvil_strbuf_append(&be->code, "         ST    R2,72(,R13)       Store int to temp\n");
            if (instr->result && instr->result->type && 
                instr->result->type->kind == ANVIL_TYPE_F32) {
                /* For short FP, we need to use unnormalized then normalize */
                anvil_strbuf_append(&be->code, "         L     R2,72(,R13)       Reload integer\n");
                anvil_strbuf_append(&be->code, "         ST    R2,76(,R13)       Store for conversion\n");
                anvil_strbuf_append(&be->code, "         SDR   0,0               Clear F0\n");
                anvil_strbuf_append(&be->code, "         LD    0,=D'0'           Load zero\n");
                anvil_strbuf_append(&be->code, "         AW    0,76(,R13)        Add unnormalized word\n");
            } else {
                /* For long FP */
                anvil_strbuf_append(&be->code, "         SDR   0,0               Clear F0\n");
                anvil_strbuf_append(&be->code, "         LD    0,=D'0'           Load zero\n");
                anvil_strbuf_append(&be->code, "         AW    0,72(,R13)        Add unnormalized word\n");
            }
            break;
            
        case ANVIL_OP_FPTOSI:
            /* Floating-point to signed integer (HFP conversion) */
            s370_emit_load_fp_value(be, instr->operands[0], S370_F0);
            /* HFP to integer conversion using "Magic Number" technique:
             * Add X'4E00000000000000' to shift mantissa so integer part
             * ends up in the low 32 bits of the 64-bit result.
             * The exponent 4E (hex) = 78 (dec), which represents 16^(78-64) = 16^14
             * This aligns the binary point to extract the integer portion. */
            anvil_strbuf_append(&be->code, "         AW    0,=X'4E00000000000000' Add magic number\n");
            anvil_strbuf_append(&be->code, "         STD   0,80(,R13)        Store result to temp\n");
            anvil_strbuf_append(&be->code, "         L     R15,84(,R13)      Load integer from low word\n");
            break;
            
        case ANVIL_OP_FPEXT:
            /* Extend short FP to long FP */
            s370_emit_load_fp_value(be, instr->operands[0], S370_F0);
            /* Short to long is automatic - just load short and use as long */
            anvil_strbuf_append(&be->code, "         SDR   2,2               Clear F2\n");
            anvil_strbuf_append(&be->code, "         LER   2,0               Copy short to F2\n");
            anvil_strbuf_append(&be->code, "         LDR   0,2               F0 now has long FP\n");
            break;
            
        case ANVIL_OP_FPTRUNC:
            /* Truncate long FP to short FP */
            s370_emit_load_fp_value(be, instr->operands[0], S370_F0);
            /* Long to short - use LRER to round */
            anvil_strbuf_append(&be->code, "         LRER  0,0               Round long to short\n");
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "*        Unimplemented op %d\n", instr->op);
            break;
    }
}

static void s370_emit_block(s370_backend_t *be, anvil_block_t *block)
{
    if (!block) return;
    
    /* Block label - use unique label with function name prefix to avoid duplicates */
    char upper_func[64], upper_block[64];
    s370_uppercase(upper_func, be->current_func, sizeof(upper_func));
    s370_uppercase(upper_block, block->name, sizeof(upper_block));
    anvil_strbuf_appendf(&be->code, "%s$%s DS    0H\n", upper_func, upper_block);
    
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        s370_emit_instr(be, instr);
    }
}

static void s370_emit_func_dynsize(s370_backend_t *be, anvil_func_t *func)
{
    /* Calculate total stack frame size:
     * 72 (Save Area) + 16 (FP temps) + local_vars + (max_call_args * 4)
     * Align to doubleword (8 bytes) for performance */
    int total_size = DYN_LOCALS_OFFSET + be->local_vars_size + (be->max_call_args * 4);
    
    /* Align to 8 bytes */
    if (total_size % 8 != 0) {
        total_size += (8 - (total_size % 8));
    }
    
    /* Minimum size includes SA + FP temp areas */
    if (total_size < DYN_LOCALS_OFFSET) total_size = DYN_LOCALS_OFFSET;
    
    char upper_name[64];
    s370_uppercase(upper_name, func->name, sizeof(upper_name));
    anvil_strbuf_appendf(&be->code, "DYN@%-4s EQU   %d                 Stack frame size for %s\n", 
        upper_name, total_size, upper_name);
}

static void s370_emit_func(s370_backend_t *be, anvil_func_t *func)
{
    if (!func) return;
    
    /* Reset per-function state */
    be->local_vars_size = 0;
    be->max_call_args = 0;
    be->num_stack_slots = 0;  /* Reset stack slots for new function */
    
    s370_emit_prologue(be, func);
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        s370_emit_block(be, block);
    }
    
    /* Store calculated stack size */
    func->stack_size = SA_SIZE + be->local_vars_size + (be->max_call_args * 4);
    
    /* Drop base register - good practice before next function */
    anvil_strbuf_append(&be->code, "*\n");
    anvil_strbuf_append(&be->code, "         DROP  R12\n");
    anvil_strbuf_append(&be->code, "*\n");
}

static void s370_emit_footer(s370_backend_t *be, const char *entry_point)
{
    /* Literal pool */
    anvil_strbuf_append(&be->code, "*\n");
    anvil_strbuf_append(&be->code, "         LTORG                    Literal pool\n");
    
    /* Register equates */
    anvil_strbuf_append(&be->code, "*\n");
    anvil_strbuf_append(&be->code, "*        Register equates\n");
    for (int i = 0; i < 16; i++) {
        anvil_strbuf_appendf(&be->code, "R%-7d EQU   %d\n", i, i);
    }
    
    anvil_strbuf_append(&be->code, "*\n");
    if (entry_point) {
        char upper_entry[64];
        s370_uppercase(upper_entry, entry_point, sizeof(upper_entry));
        anvil_strbuf_appendf(&be->code, "         END   %s\n", upper_entry);
    } else {
        anvil_strbuf_append(&be->code, "         END\n");
    }
}

static anvil_error_t s370_codegen_module(anvil_backend_t *be, anvil_module_t *mod,
                                          char **output, size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;
    
    s370_backend_t *priv = be->priv;
    const char *entry_point = NULL;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    
    /* Reset string table */
    priv->num_strings = 0;
    priv->string_counter = 0;
    
    s370_emit_header(priv, mod->name);
    
    /* Emit code for all functions (skip declarations) */
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (!func->is_declaration) {
            s370_emit_func(priv, func);
            /* First non-declaration function is entry point */
            if (!entry_point) entry_point = func->name;
        }
    }
    
    /* Emit dynamic area size equates for each function (skip declarations) */
    anvil_strbuf_append(&priv->code, "*\n");
    anvil_strbuf_append(&priv->code, "*        Dynamic area sizes (for GETMAIN/FREEMAIN)\n");
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (func->is_declaration) continue;
        /* Need to recalculate - store in func or re-emit */
        priv->local_vars_size = 0;
        priv->max_call_args = 0;
        /* Scan function to get max_call_args and count local variables */
        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
                if (instr->op == ANVIL_OP_CALL) {
                    int num_args = (int)(instr->num_operands - 1);
                    if (num_args > priv->max_call_args) {
                        priv->max_call_args = num_args;
                    }
                } else if (instr->op == ANVIL_OP_ALLOCA) {
                    priv->local_vars_size += 4;  /* 4 bytes per local var */
                }
            }
        }
        s370_emit_func_dynsize(priv, func);
    }
    
    /* Emit global variables (these are still static - not in dynamic area) */
    if (mod->num_globals > 0) {
        anvil_strbuf_append(&priv->code, "*\n");
        anvil_strbuf_append(&priv->code, "*        Global variables (static)\n");
        for (anvil_global_t *g = mod->globals; g; g = g->next) {
            anvil_strbuf_appendf(&priv->code, "%-8s DS    F                  Global variable\n",
                g->value->name);
        }
    }
    
    /* Emit string constants */
    if (priv->num_strings > 0) {
        anvil_strbuf_append(&priv->code, "*\n");
        anvil_strbuf_append(&priv->code, "*        String constants\n");
        for (size_t i = 0; i < priv->num_strings; i++) {
            s370_string_entry_t *entry = &priv->strings[i];
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
                        anvil_strbuf_append(&priv->code, "''");  /* Double single quotes */
                    } else if (*p == '&') {
                        anvil_strbuf_append(&priv->code, "&&");  /* Double ampersands */
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
    
    s370_emit_footer(priv, entry_point);
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

static anvil_error_t s370_codegen_func(anvil_backend_t *be, anvil_func_t *func,
                                        char **output, size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    
    s370_backend_t *priv = be->priv;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);
    
    s370_emit_func(priv, func);
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

const anvil_backend_ops_t anvil_backend_s370 = {
    .name = "S/370",
    .arch = ANVIL_ARCH_S370,
    .init = s370_init,
    .cleanup = s370_cleanup,
    .codegen_module = s370_codegen_module,
    .codegen_func = s370_codegen_func,
    .get_arch_info = s370_get_arch_info
};
