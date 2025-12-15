/*
 * ANVIL - IBM S/390 Backend
 * 
 * Big-endian, stack grows UPWARD (toward higher addresses)
 * 31-bit addressing mode (high bit reserved for addressing mode flag)
 * Generates HLASM (High Level Assembler) syntax
 * 
 * Differences from S/370:
 *   - 31-bit addresses (bit 0 is AMODE flag)
 *   - More floating point registers (16 FPRs)
 *   - Additional instructions (MVCLE, CLCLE, MSR, etc.)
 *   - Relative branch instructions (J, JE, JNE, etc.)
 *   - LHI (Load Halfword Immediate)
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
 * Parameter passing (MVS standard):
 *   R1 points to a list of fullword addresses
 *   Each address points to the actual parameter value
 *   High-order bit of last address is set to 1
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* S/390 registers */
static const char *s390_reg_names[] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
    "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"
};

/* S/390 has 16 FPRs (0-15), but HFP uses pairs: 0,2,4,6 for long */
#define S390_F0  0
#define S390_F2  2
#define S390_F4  4
#define S390_F6  6

/* Register usage - same as S/370 */
#define S390_R0   0
#define S390_R1   1
#define S390_R2   2
#define S390_R3   3
#define S390_R12  12
#define S390_R13  13
#define S390_R14  14
#define S390_R15  15

#define SA_SIZE   72

/* Dynamic storage layout (relative to R13):
 * +0   - Save Area (72 bytes)
 * +72  - FP temp area (8 bytes for double)
 * +80  - FP temp area 2 (8 bytes for conversions)
 * +88  - Local variables start
 * +N   - Parameter list for outgoing calls
 */
#define FP_TEMP_OFFSET     72
#define FP_TEMP2_OFFSET    80
#define DYN_LOCALS_OFFSET  88

/* String table entry */
typedef struct {
    const char *str;
    char label[16];
    size_t len;
} s390_string_entry_t;

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
    s390_string_entry_t *strings;
    size_t num_strings;
    size_t strings_cap;
} s390_backend_t;

static const anvil_arch_info_t s390_arch_info = {
    .arch = ANVIL_ARCH_S390,
    .name = "S/390",
    .ptr_size = 4,
    .addr_bits = 31,
    .word_size = 4,
    .num_gpr = 16,
    .num_fpr = 16,
    .endian = ANVIL_ENDIAN_BIG,
    .stack_dir = ANVIL_STACK_UP,
    .has_condition_codes = true,
    .has_delay_slots = false
};

static anvil_error_t s390_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    s390_backend_t *priv = calloc(1, sizeof(s390_backend_t));
    if (!priv) return ANVIL_ERR_NOMEM;
    
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->csect_name = "CODE";
    priv->ctx = ctx;  /* Save context for FP format selection */
    
    be->priv = priv;
    return ANVIL_OK;
}

static void s390_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    s390_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv->strings);
    free(priv->stack_slots);
    free(priv);
    be->priv = NULL;
}

/* Get stack slot offset for an ALLOCA result value */
static int s390_get_stack_slot(s390_backend_t *be, anvil_value_t *val)
{
    for (size_t i = 0; i < be->num_stack_slots; i++) {
        if (be->stack_slots[i].value == val) {
            return be->stack_slots[i].offset;
        }
    }
    return -1;
}

/* Add a stack slot for an ALLOCA */
static int s390_add_stack_slot(s390_backend_t *be, anvil_value_t *val)
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
    be->local_vars_size += 4;
    
    return offset;
}

static const anvil_arch_info_t *s390_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &s390_arch_info;
}

/* Convert function name to uppercase (GCCMVS convention) */
static void s390_uppercase(char *dest, const char *src, size_t max_len)
{
    size_t i;
    for (i = 0; i < max_len - 1 && src[i]; i++) {
        dest[i] = (src[i] >= 'a' && src[i] <= 'z') ? src[i] - 32 : src[i];
    }
    dest[i] = '\0';
}

static void s390_emit_header(s390_backend_t *be, const char *module_name)
{
    (void)module_name;
    anvil_strbuf_append(&be->code, "***********************************************************************\n");
    anvil_strbuf_appendf(&be->code, "*        Generated by ANVIL for IBM S/390\n");
    anvil_strbuf_append(&be->code, "***********************************************************************\n");
    anvil_strbuf_append(&be->code, "         CSECT\n");
    anvil_strbuf_append(&be->code, "         AMODE ANY\n");
    anvil_strbuf_append(&be->code, "         RMODE ANY\n");
    anvil_strbuf_append(&be->code, "*\n");
}

static void s390_emit_prologue(s390_backend_t *be, anvil_func_t *func)
{
    char upper_name[64];
    s390_uppercase(upper_name, func->name, sizeof(upper_name));
    be->current_func = func->name;
    
    /* Entry point label (uppercase) */
    anvil_strbuf_appendf(&be->code, "%-8s DS    0H\n", upper_name);
    
    /* 1. Save caller's registers */
    anvil_strbuf_append(&be->code, "         STM   R14,R12,12(R13)    Save caller's registers\n");
    
    /* 2. Establish addressability */
    anvil_strbuf_append(&be->code, "         LR    R12,R15            Copy entry point to base reg\n");
    anvil_strbuf_appendf(&be->code, "         USING %s,R12            Establish addressability\n", upper_name);
    
    /* 3. Save R1 (param pointer) */
    anvil_strbuf_append(&be->code, "         LR    R11,R1             Save parameter list pointer\n");
    
    /* 4. Set up save area chain (stack allocation, no GETMAIN) */
    anvil_strbuf_append(&be->code, "*        Set up save area chain (stack allocation)\n");
    anvil_strbuf_append(&be->code, "         LA    R2,72(,R13)        R2 -> our save area\n");
    anvil_strbuf_append(&be->code, "         ST    R13,4(,R2)         Chain: new->prev = caller's\n");
    anvil_strbuf_append(&be->code, "         ST    R2,8(,R13)         Chain: caller->next = new\n");
    anvil_strbuf_append(&be->code, "         LR    R13,R2             R13 -> our save area\n");
    anvil_strbuf_append(&be->code, "*\n");
}

static void s390_emit_epilogue(s390_backend_t *be)
{
    (void)be;
    anvil_strbuf_append(&be->code, "*        Function epilogue\n");
    
    /* 1. Restore caller's SA pointer */
    anvil_strbuf_append(&be->code, "         L     R13,4(,R13)        Restore caller's SA pointer\n");
    
    /* 2. Restore registers - R15 has return value */
    anvil_strbuf_append(&be->code, "         L     R14,12(,R13)       Restore return address\n");
    anvil_strbuf_append(&be->code, "         LM    R0,R12,20(,R13)    Restore R0-R12\n");
    anvil_strbuf_append(&be->code, "         BR    R14                Return to caller\n");
}

/* Add string to string table and return its label */
static const char *s390_add_string(s390_backend_t *be, const char *str)
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
        s390_string_entry_t *new_strings = realloc(be->strings, 
            new_cap * sizeof(s390_string_entry_t));
        if (!new_strings) return "STR$ERR";
        be->strings = new_strings;
        be->strings_cap = new_cap;
    }
    
    /* Add new string */
    s390_string_entry_t *entry = &be->strings[be->num_strings];
    entry->str = str;
    entry->len = strlen(str);
    snprintf(entry->label, sizeof(entry->label), "STR$%d", be->string_counter++);
    be->num_strings++;
    
    return entry->label;
}

/* Emit floating-point value to FP register (HFP or IEEE based on context) */
static void s390_emit_load_fp_value(s390_backend_t *be, anvil_value_t *val, int target_fpr, anvil_ctx_t *ctx)
{
    if (!val) return;
    
    bool use_ieee = (ctx && ctx->fp_format == ANVIL_FP_IEEE754);
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_FLOAT:
            /* Load FP constant using literal */
            if (val->type && val->type->kind == ANVIL_TYPE_F32) {
                if (use_ieee) {
                    /* IEEE short - use LE with IEEE literal */
                    anvil_strbuf_appendf(&be->code, "         LE    %d,=EB'%g'        Load IEEE short FP\n",
                        target_fpr, val->data.f);
                } else {
                    /* HFP short */
                    anvil_strbuf_appendf(&be->code, "         LE    %d,=E'%g'         Load HFP short FP\n",
                        target_fpr, val->data.f);
                }
            } else {
                if (use_ieee) {
                    /* IEEE long - use LD with IEEE literal */
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
            /* FP result - assume in F0 */
            if (target_fpr != S390_F0) {
                if (val->type && val->type->kind == ANVIL_TYPE_F32) {
                    anvil_strbuf_appendf(&be->code, "         LER   %d,0             Copy short FP result\n", target_fpr);
                } else {
                    anvil_strbuf_appendf(&be->code, "         LDR   %d,0             Copy long FP result\n", target_fpr);
                }
            }
            break;
            
        case ANVIL_VAL_PARAM:
            /* FP parameter - load from parameter area */
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

static void s390_emit_load_value(s390_backend_t *be, anvil_value_t *val, int target_reg)
{
    if (!val) return;
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_INT:
            if (val->data.i >= 0 && val->data.i <= 4095) {
                /* Use LA for small positive constants (0-4095) */
                anvil_strbuf_appendf(&be->code, "         LA    %s,%lld            Load constant\n",
                    s390_reg_names[target_reg], (long long)val->data.i);
            } else if (val->data.i >= -32768 && val->data.i <= 32767) {
                /* S/390 has LHI - Load Halfword Immediate */
                anvil_strbuf_appendf(&be->code, "         LHI   %s,%lld           Load constant\n",
                    s390_reg_names[target_reg], (long long)val->data.i);
            } else {
                /* Use L with fullword literal */
                anvil_strbuf_appendf(&be->code, "         L     %s,=F'%lld'       Load constant\n",
                    s390_reg_names[target_reg], (long long)val->data.i);
            }
            break;
            
        case ANVIL_VAL_CONST_STRING:
            {
                /* Add string to table and load its address */
                const char *label = s390_add_string(be, val->data.str ? val->data.str : "");
                anvil_strbuf_appendf(&be->code, "         LA    %s,%s            Load string address\n",
                    s390_reg_names[target_reg], label);
            }
            break;
            
        case ANVIL_VAL_PARAM:
            /* MVS parameter passing: R11 has saved R1 (param list pointer) */
            /* R1 points to list of ADDRESSES, each address points to the value */
            /* Step 1: Load address of parameter from list */
            anvil_strbuf_appendf(&be->code, "         L     %s,%zu(,R11)       Load addr of param %zu\n",
                s390_reg_names[target_reg], val->data.param.index * 4, val->data.param.index);
            /* Note: We do NOT clear the VL bit - allows full 31/64-bit addressing */
            /* Step 2: Load actual value from that address */
            anvil_strbuf_appendf(&be->code, "         L     %s,0(,%s)         Load param value\n",
                s390_reg_names[target_reg], s390_reg_names[target_reg]);
            break;
            
        case ANVIL_VAL_INSTR:
            /* Check if this is an ALLOCA result - use stack slot offset (address) */
            if (val->data.instr && val->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = s390_get_stack_slot(be, val);
                if (offset >= 0) {
                    anvil_strbuf_appendf(&be->code, "         LA    %s,%d(,R13)       Load addr of local var\n",
                        s390_reg_names[target_reg], offset);
                    break;
                }
            }
            /* Check if this is a LOAD from a stack slot - load value directly */
            if (val->data.instr && val->data.instr->op == ANVIL_OP_LOAD) {
                anvil_instr_t *load_instr = val->data.instr;
                if (load_instr->operands[0]->kind == ANVIL_VAL_INSTR &&
                    load_instr->operands[0]->data.instr &&
                    load_instr->operands[0]->data.instr->op == ANVIL_OP_ALLOCA) {
                    int offset = s390_get_stack_slot(be, load_instr->operands[0]);
                    if (offset >= 0) {
                        anvil_strbuf_appendf(&be->code, "         L     %s,%d(,R13)       Load value from stack slot\n",
                            s390_reg_names[target_reg], offset);
                        break;
                    }
                }
            }
            /* Otherwise, result in R15 by convention */
            if (target_reg != S390_R15) {
                anvil_strbuf_appendf(&be->code, "         LR    %s,R15            Copy result\n",
                    s390_reg_names[target_reg]);
            }
            break;
            
        case ANVIL_VAL_GLOBAL:
            anvil_strbuf_appendf(&be->code, "         L     %s,%s            Load global\n",
                s390_reg_names[target_reg], val->name);
            break;
            
        case ANVIL_VAL_FUNC:
            /* Load function entry point address */
            anvil_strbuf_appendf(&be->code, "         L     %s,=V(%s)        Load function address\n",
                s390_reg_names[target_reg], val->name);
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "*        Unknown value kind %d\n", val->kind);
            break;
    }
}

static void s390_emit_instr(s390_backend_t *be, anvil_instr_t *instr)
{
    if (!instr) return;
    
    switch (instr->op) {
        case ANVIL_OP_ALLOCA:
            /* Allocate space in dynamic area for local variable */
            {
                int offset = s390_add_stack_slot(be, instr->result);
                anvil_strbuf_appendf(&be->code, "         XC    %d(4,R13),%d(R13)  Init local var to 0\n", 
                    offset, offset);
            }
            break;
            
        case ANVIL_OP_ADD:
            /* Optimization: use AHI for small immediate constants */
            if (instr->operands[1]->kind == ANVIL_VAL_CONST_INT &&
                instr->operands[1]->data.i >= -32768 && instr->operands[1]->data.i <= 32767) {
                s390_emit_load_value(be, instr->operands[0], S390_R2);
                anvil_strbuf_appendf(&be->code, "         AHI   R2,%lld            Add halfword immediate\n",
                    (long long)instr->operands[1]->data.i);
                anvil_strbuf_append(&be->code, "         LR    R15,R2            Result in R15\n");
            } else {
                s390_emit_load_value(be, instr->operands[0], S390_R2);
                s390_emit_load_value(be, instr->operands[1], S390_R3);
                anvil_strbuf_append(&be->code, "         AR    R2,R3             Add registers\n");
                anvil_strbuf_append(&be->code, "         LR    R15,R2            Result in R15\n");
            }
            break;
            
        case ANVIL_OP_SUB:
            s390_emit_load_value(be, instr->operands[0], S390_R2);
            s390_emit_load_value(be, instr->operands[1], S390_R3);
            anvil_strbuf_append(&be->code, "         SR    R2,R3             Subtract registers\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2            Result in R15\n");
            break;
            
        case ANVIL_OP_MUL:
            s390_emit_load_value(be, instr->operands[0], S390_R2);
            s390_emit_load_value(be, instr->operands[1], S390_R3);
            /* S/390 has MSR - Multiply Single Register */
            anvil_strbuf_append(&be->code, "         MSR   R2,R3             Multiply single\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2            Result in R15\n");
            break;
            
        case ANVIL_OP_SDIV:
            s390_emit_load_value(be, instr->operands[0], S390_R3);
            anvil_strbuf_append(&be->code, "         SRDA  R2,32             Sign extend R3 into R2:R3\n");
            s390_emit_load_value(be, instr->operands[1], S390_R0);
            anvil_strbuf_append(&be->code, "         DR    R2,R0             Divide R2:R3 by R0\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R3            Quotient to R15\n");
            break;
            
        case ANVIL_OP_SMOD:
            s390_emit_load_value(be, instr->operands[0], S390_R3);
            anvil_strbuf_append(&be->code, "         SRDA  R2,32             Sign extend\n");
            s390_emit_load_value(be, instr->operands[1], S390_R0);
            anvil_strbuf_append(&be->code, "         DR    R2,R0             Divide\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2            Remainder to R15\n");
            break;
            
        case ANVIL_OP_AND:
            s390_emit_load_value(be, instr->operands[0], S390_R2);
            s390_emit_load_value(be, instr->operands[1], S390_R3);
            anvil_strbuf_append(&be->code, "         NR    R2,R3             AND registers\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2\n");
            break;
            
        case ANVIL_OP_OR:
            s390_emit_load_value(be, instr->operands[0], S390_R2);
            s390_emit_load_value(be, instr->operands[1], S390_R3);
            anvil_strbuf_append(&be->code, "         OR    R2,R3             OR registers\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2\n");
            break;
            
        case ANVIL_OP_XOR:
            s390_emit_load_value(be, instr->operands[0], S390_R2);
            s390_emit_load_value(be, instr->operands[1], S390_R3);
            anvil_strbuf_append(&be->code, "         XR    R2,R3             XOR registers\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2\n");
            break;
            
        case ANVIL_OP_SHL:
            s390_emit_load_value(be, instr->operands[0], S390_R2);
            s390_emit_load_value(be, instr->operands[1], S390_R3);
            anvil_strbuf_append(&be->code, "         SLL   R2,0(R3)          Shift left logical\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2\n");
            break;
            
        case ANVIL_OP_SHR:
            s390_emit_load_value(be, instr->operands[0], S390_R2);
            s390_emit_load_value(be, instr->operands[1], S390_R3);
            anvil_strbuf_append(&be->code, "         SRL   R2,0(R3)          Shift right logical\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2\n");
            break;
            
        case ANVIL_OP_SAR:
            s390_emit_load_value(be, instr->operands[0], S390_R2);
            s390_emit_load_value(be, instr->operands[1], S390_R3);
            anvil_strbuf_append(&be->code, "         SRA   R2,0(R3)          Shift right arithmetic\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2\n");
            break;
            
        case ANVIL_OP_NEG:
            s390_emit_load_value(be, instr->operands[0], S390_R2);
            anvil_strbuf_append(&be->code, "         LCR   R15,R2            Load complement\n");
            break;
            
        case ANVIL_OP_NOT:
            s390_emit_load_value(be, instr->operands[0], S390_R2);
            anvil_strbuf_append(&be->code, "         X     R2,=F'-1'         XOR with all 1s\n");
            anvil_strbuf_append(&be->code, "         LR    R15,R2\n");
            break;
            
        case ANVIL_OP_LOAD:
            /* Check if loading from an ALLOCA (stack slot) */
            if (instr->operands[0]->kind == ANVIL_VAL_INSTR &&
                instr->operands[0]->data.instr &&
                instr->operands[0]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = s390_get_stack_slot(be, instr->operands[0]);
                if (offset >= 0) {
                    anvil_strbuf_appendf(&be->code, "         L     R15,%d(,R13)       Load from stack slot\n", offset);
                    break;
                }
            }
            s390_emit_load_value(be, instr->operands[0], S390_R2);
            anvil_strbuf_append(&be->code, "         L     R15,0(,R2)        Load from address\n");
            break;
            
        case ANVIL_OP_STORE:
            /* Check if storing to an ALLOCA (stack slot) */
            if (instr->operands[1]->kind == ANVIL_VAL_INSTR &&
                instr->operands[1]->data.instr &&
                instr->operands[1]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = s390_get_stack_slot(be, instr->operands[1]);
                if (offset >= 0) {
                    s390_emit_load_value(be, instr->operands[0], S390_R2);
                    anvil_strbuf_appendf(&be->code, "         ST    R2,%d(,R13)        Store to stack slot\n", offset);
                    break;
                }
            }
            s390_emit_load_value(be, instr->operands[0], S390_R2);
            s390_emit_load_value(be, instr->operands[1], S390_R3);
            anvil_strbuf_append(&be->code, "         ST    R2,0(,R3)         Store to address\n");
            break;
            
        case ANVIL_OP_STRUCT_GEP:
            /* Get Struct Field Pointer - compute address of struct field */
            {
                s390_emit_load_value(be, instr->operands[0], S390_R2);
                
                int offset = 0;
                if (instr->aux_type && instr->aux_type->kind == ANVIL_TYPE_STRUCT &&
                    instr->num_operands > 1 && instr->operands[1]->kind == ANVIL_VAL_CONST_INT) {
                    unsigned field_idx = (unsigned)instr->operands[1]->data.i;
                    if (field_idx < instr->aux_type->data.struc.num_fields) {
                        offset = (int)instr->aux_type->data.struc.offsets[field_idx];
                    }
                }
                
                if (offset == 0) {
                    anvil_strbuf_append(&be->code, "         LR    R15,R2             Struct field at offset 0\n");
                } else if (offset > 0 && offset < 4096) {
                    anvil_strbuf_appendf(&be->code, "         LA    R15,%d(,R2)        Struct field at offset %d\n", offset, offset);
                } else {
                    anvil_strbuf_appendf(&be->code, "         LR    R15,R2             Load base\n");
                    anvil_strbuf_appendf(&be->code, "         AHI   R15,%d             Add field offset\n", offset);
                }
            }
            break;
            
        case ANVIL_OP_GEP:
            /* Get Element Pointer - compute address of array element */
            {
                s390_emit_load_value(be, instr->operands[0], S390_R2);
                
                if (instr->num_operands > 1) {
                    s390_emit_load_value(be, instr->operands[1], S390_R3);
                    
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
                    
                    if (elem_size == 2) {
                        anvil_strbuf_append(&be->code, "         SLL   R3,1               Index * 2\n");
                    } else if (elem_size == 4) {
                        anvil_strbuf_append(&be->code, "         SLL   R3,2               Index * 4\n");
                    } else if (elem_size == 8) {
                        anvil_strbuf_append(&be->code, "         SLL   R3,3               Index * 8\n");
                    } else if (elem_size != 1) {
                        anvil_strbuf_appendf(&be->code, "         MH    R3,=H'%d'          Index * %d\n", elem_size, elem_size);
                    }
                    
                    anvil_strbuf_append(&be->code, "         AR    R2,R3              Base + offset\n");
                }
                
                anvil_strbuf_append(&be->code, "         LR    R15,R2             Result pointer\n");
            }
            break;
            
        case ANVIL_OP_BR:
            {
                char upper_func[64], upper_block[64];
                s390_uppercase(upper_func, be->current_func, sizeof(upper_func));
                s390_uppercase(upper_block, instr->true_block->name, sizeof(upper_block));
                anvil_strbuf_appendf(&be->code, "         J     %s$%s            Branch relative\n",
                    upper_func, upper_block);
            }
            break;
            
        case ANVIL_OP_BR_COND:
            {
                char upper_func[64], upper_true[64], upper_false[64];
                s390_uppercase(upper_func, be->current_func, sizeof(upper_func));
                s390_uppercase(upper_true, instr->true_block->name, sizeof(upper_true));
                s390_uppercase(upper_false, instr->false_block->name, sizeof(upper_false));
                s390_emit_load_value(be, instr->operands[0], S390_R2);
                anvil_strbuf_append(&be->code, "         LTR   R2,R2             Test register\n");
                anvil_strbuf_appendf(&be->code, "         JNZ   %s$%s            Branch if not zero\n",
                    upper_func, upper_true);
                anvil_strbuf_appendf(&be->code, "         J     %s$%s            Branch to else\n",
                    upper_func, upper_false);
            }
            break;
            
        case ANVIL_OP_RET:
            if (instr->num_operands > 0) {
                s390_emit_load_value(be, instr->operands[0], S390_R15);
            } else {
                anvil_strbuf_append(&be->code, "         SR    R15,R15           Return 0\n");
            }
            s390_emit_epilogue(be);
            break;
            
        case ANVIL_OP_CALL:
            {
                int num_args = (int)(instr->num_operands - 1);
                if (num_args > be->max_call_args) {
                    be->max_call_args = num_args;
                }
                
                int parm_base = DYN_LOCALS_OFFSET + be->local_vars_size;
                
                anvil_strbuf_append(&be->code, "*        Call setup (reentrant)\n");
                for (size_t i = 1; i < instr->num_operands; i++) {
                    s390_emit_load_value(be, instr->operands[i], S390_R0);
                    int parm_offset = parm_base + ((int)(i - 1) * 4);
                    anvil_strbuf_appendf(&be->code, "         ST    R0,%d(,R13)       Store param %zu\n", 
                        parm_offset, i - 1);
                }
                
                if (num_args > 0) {
                    anvil_strbuf_appendf(&be->code, "         LA    R1,%d(,R13)       R1 -> param list\n", parm_base);
                    int last_parm_offset = parm_base + ((num_args - 1) * 4);
                    anvil_strbuf_appendf(&be->code, "         OI    %d(R13),X'80'     Mark last param (VL)\n", 
                        last_parm_offset);
                }
                
                {
                    char upper_callee[64];
                    s390_uppercase(upper_callee, instr->operands[0]->name, sizeof(upper_callee));
                    anvil_strbuf_appendf(&be->code, "         L     R15,=V(%s)        Load entry point\n", upper_callee);
                }
                anvil_strbuf_append(&be->code, "         BASR  R14,R15           Call subroutine\n");
                
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
                    case ANVIL_OP_CMP_EQ: branch_cond = "JE"; break;
                    case ANVIL_OP_CMP_NE: branch_cond = "JNE"; break;
                    case ANVIL_OP_CMP_LT: branch_cond = "JL"; break;
                    case ANVIL_OP_CMP_LE: branch_cond = "JNH"; break;
                    case ANVIL_OP_CMP_GT: branch_cond = "JH"; break;
                    case ANVIL_OP_CMP_GE: branch_cond = "JNL"; break;
                    default: branch_cond = "JE"; break;
                }
                
                s390_emit_load_value(be, instr->operands[0], S390_R2);
                s390_emit_load_value(be, instr->operands[1], S390_R3);
                anvil_strbuf_append(&be->code, "         CR    R2,R3             Compare registers\n");
                anvil_strbuf_append(&be->code, "         LHI   R15,1             Assume true\n");
                /* S/390 relative branch Jxx is 4 bytes, SR is 2 bytes = 6 total */
                anvil_strbuf_appendf(&be->code, "         %-5s *+6               Skip if condition met\n", branch_cond);
                anvil_strbuf_append(&be->code, "         SR    R15,R15           Set false\n");
            }
            break;
            
        /* ================================================================
         * Floating-Point Operations (HFP or IEEE 754 based on context)
         * S/390 has 16 FPRs (0-15)
         * ================================================================ */
        
        case ANVIL_OP_FADD:
            s390_emit_load_fp_value(be, instr->operands[0], S390_F0, be->ctx);
            s390_emit_load_fp_value(be, instr->operands[1], S390_F2, be->ctx);
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
            s390_emit_load_fp_value(be, instr->operands[0], S390_F0, be->ctx);
            s390_emit_load_fp_value(be, instr->operands[1], S390_F2, be->ctx);
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
            s390_emit_load_fp_value(be, instr->operands[0], S390_F0, be->ctx);
            s390_emit_load_fp_value(be, instr->operands[1], S390_F2, be->ctx);
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
            s390_emit_load_fp_value(be, instr->operands[0], S390_F0, be->ctx);
            s390_emit_load_fp_value(be, instr->operands[1], S390_F2, be->ctx);
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
            s390_emit_load_fp_value(be, instr->operands[0], S390_F0, be->ctx);
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
            s390_emit_load_fp_value(be, instr->operands[0], S390_F0, be->ctx);
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
            s390_emit_load_fp_value(be, instr->operands[0], S390_F0, be->ctx);
            s390_emit_load_fp_value(be, instr->operands[1], S390_F2, be->ctx);
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
            anvil_strbuf_append(&be->code, "         LHI   R15,1             Assume true\n");
            anvil_strbuf_append(&be->code, "         JE    *+6               Skip if equal\n");
            anvil_strbuf_append(&be->code, "         SR    R15,R15           Set false\n");
            break;
            
        case ANVIL_OP_SITOFP:
            s390_emit_load_value(be, instr->operands[0], S390_R2);
            anvil_strbuf_append(&be->code, "         ST    R2,72(,R13)       Store int to temp\n");
            anvil_strbuf_append(&be->code, "         SDR   0,0               Clear F0\n");
            anvil_strbuf_append(&be->code, "         LD    0,=D'0'           Load zero\n");
            anvil_strbuf_append(&be->code, "         AW    0,72(,R13)        Add unnormalized word\n");
            break;
            
        case ANVIL_OP_FPTOSI:
            /* Floating-point to signed integer (HFP conversion) */
            s390_emit_load_fp_value(be, instr->operands[0], S390_F0, be->ctx);
            /* HFP to integer conversion using "Magic Number" technique */
            anvil_strbuf_append(&be->code, "         AW    0,=X'4E00000000000000' Add magic number\n");
            anvil_strbuf_append(&be->code, "         STD   0,80(,R13)        Store result to temp\n");
            anvil_strbuf_append(&be->code, "         L     R15,84(,R13)      Load integer from low word\n");
            break;
            
        case ANVIL_OP_FPEXT:
            s390_emit_load_fp_value(be, instr->operands[0], S390_F0, be->ctx);
            anvil_strbuf_append(&be->code, "         SDR   2,2               Clear F2\n");
            anvil_strbuf_append(&be->code, "         LER   2,0               Copy short to F2\n");
            anvil_strbuf_append(&be->code, "         LDR   0,2               F0 now has long FP\n");
            break;
            
        case ANVIL_OP_FPTRUNC:
            s390_emit_load_fp_value(be, instr->operands[0], S390_F0, be->ctx);
            anvil_strbuf_append(&be->code, "         LRER  0,0               Round long to short\n");
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "*        Unimplemented op %d\n", instr->op);
            break;
    }
}

static void s390_emit_block(s390_backend_t *be, anvil_block_t *block)
{
    if (!block) return;
    
    /* Block label - use unique label with function name prefix to avoid duplicates */
    char upper_func[64], upper_block[64];
    s390_uppercase(upper_func, be->current_func, sizeof(upper_func));
    s390_uppercase(upper_block, block->name, sizeof(upper_block));
    anvil_strbuf_appendf(&be->code, "%s$%s DS    0H\n", upper_func, upper_block);
    
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        s390_emit_instr(be, instr);
    }
}

static void s390_emit_func_dynsize(s390_backend_t *be, anvil_func_t *func)
{
    /* Include FP temp areas in stack frame */
    int total_size = DYN_LOCALS_OFFSET + be->local_vars_size + (be->max_call_args * 4);
    if (total_size % 8 != 0) {
        total_size += (8 - (total_size % 8));
    }
    if (total_size < DYN_LOCALS_OFFSET) total_size = DYN_LOCALS_OFFSET;
    
    char upper_name[64];
    s390_uppercase(upper_name, func->name, sizeof(upper_name));
    anvil_strbuf_appendf(&be->code, "DYN@%-4s EQU   %d                 Stack frame size for %s\n", 
        upper_name, total_size, upper_name);
}

static void s390_emit_func(s390_backend_t *be, anvil_func_t *func)
{
    if (!func) return;
    
    be->local_vars_size = 0;
    be->max_call_args = 0;
    be->num_stack_slots = 0;  /* Reset stack slots for new function */
    
    s390_emit_prologue(be, func);
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        s390_emit_block(be, block);
    }
    
    func->stack_size = SA_SIZE + be->local_vars_size + (be->max_call_args * 4);
    
    /* Drop base register - good practice before next function */
    anvil_strbuf_append(&be->code, "*\n");
    anvil_strbuf_append(&be->code, "         DROP  R12\n");
    anvil_strbuf_append(&be->code, "*\n");
}

static void s390_emit_footer(s390_backend_t *be, const char *entry_point)
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
        s390_uppercase(upper_entry, entry_point, sizeof(upper_entry));
        anvil_strbuf_appendf(&be->code, "         END   %s\n", upper_entry);
    } else {
        anvil_strbuf_append(&be->code, "         END\n");
    }
}

static anvil_error_t s390_codegen_module(anvil_backend_t *be, anvil_module_t *mod,
                                          char **output, size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;
    
    s390_backend_t *priv = be->priv;
    const char *entry_point = NULL;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    
    /* Reset string table */
    priv->num_strings = 0;
    priv->string_counter = 0;
    
    s390_emit_header(priv, mod->name);
    
    /* Emit code for all functions (skip declarations) */
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (!func->is_declaration) {
            s390_emit_func(priv, func);
            if (!entry_point) entry_point = func->name;
        }
    }
    
    /* Emit dynamic area size equates (skip declarations) */
    anvil_strbuf_append(&priv->code, "*\n");
    anvil_strbuf_append(&priv->code, "*        Dynamic area sizes (for GETMAIN/FREEMAIN)\n");
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
                    priv->local_vars_size += 4;
                }
            }
        }
        s390_emit_func_dynsize(priv, func);
    }
    
    /* Emit global variables (static) */
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
            s390_string_entry_t *entry = &priv->strings[i];
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
    
    s390_emit_footer(priv, entry_point);
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

static anvil_error_t s390_codegen_func(anvil_backend_t *be, anvil_func_t *func,
                                        char **output, size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    
    s390_backend_t *priv = be->priv;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);
    
    s390_emit_func(priv, func);
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

const anvil_backend_ops_t anvil_backend_s390 = {
    .name = "S/390",
    .arch = ANVIL_ARCH_S390,
    .init = s390_init,
    .cleanup = s390_cleanup,
    .codegen_module = s390_codegen_module,
    .codegen_func = s390_codegen_func,
    .get_arch_info = s390_get_arch_info
};
