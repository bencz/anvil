/*
 * ANVIL - x86-64 Backend
 * 
 * Little-endian, stack grows downward
 * Generates GAS or NASM syntax
 * Uses System V AMD64 ABI by default
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* x86-64 registers */
static const char *x64_gpr64_names[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};

static const char *x64_gpr32_names[] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
};

static const char *x64_gpr16_names[] = {
    "ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
    "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"
};

static const char *x64_gpr8_names[] = {
    "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
    "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"
};

/* System V AMD64 ABI: argument registers */
static const int sysv_arg_regs[] = { 7, 6, 2, 1, 8, 9 }; /* rdi, rsi, rdx, rcx, r8, r9 */
#define SYSV_NUM_ARG_REGS 6

/* Register indices */
#define X64_RAX 0
#define X64_RCX 1
#define X64_RDX 2
#define X64_RBX 3
#define X64_RSP 4
#define X64_RBP 5
#define X64_RSI 6
#define X64_RDI 7
#define X64_R8  8
#define X64_R9  9
#define X64_R10 10
#define X64_R11 11
#define X64_R12 12
#define X64_R13 13
#define X64_R14 14
#define X64_R15 15

/* String table entry */
typedef struct {
    const char *str;
    char label[16];
    size_t len;
} x64_string_entry_t;

/* Stack slot for local variables */
typedef struct {
    anvil_value_t *value;
    int offset;
} x64_stack_slot_t;

/* Backend private data */
typedef struct {
    anvil_strbuf_t code;
    anvil_strbuf_t data;
    anvil_syntax_t syntax;
    int label_counter;
    int string_counter;
    int stack_offset;
    int next_stack_offset;
    
    /* Stack slots for local variables */
    x64_stack_slot_t *stack_slots;
    size_t num_stack_slots;
    size_t stack_slots_cap;
    
    /* String table */
    x64_string_entry_t *strings;
    size_t num_strings;
    size_t strings_cap;
    
    /* Current function being generated */
    anvil_func_t *current_func;
} x64_backend_t;

static const anvil_arch_info_t x64_arch_info = {
    .arch = ANVIL_ARCH_X86_64,
    .name = "x86-64",
    .ptr_size = 8,
    .addr_bits = 64,
    .word_size = 8,
    .num_gpr = 16,
    .num_fpr = 16,
    .endian = ANVIL_ENDIAN_LITTLE,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false
};

static anvil_error_t x64_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    x64_backend_t *priv = calloc(1, sizeof(x64_backend_t));
    if (!priv) return ANVIL_ERR_NOMEM;
    
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->syntax = ctx->syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS : ctx->syntax;
    
    be->priv = priv;
    return ANVIL_OK;
}

static void x64_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    x64_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv->strings);
    free(priv->stack_slots);
    free(priv);
    be->priv = NULL;
}

/* Add stack slot for local variable */
static int x64_add_stack_slot(x64_backend_t *be, anvil_value_t *val)
{
    if (be->num_stack_slots >= be->stack_slots_cap) {
        size_t new_cap = be->stack_slots_cap ? be->stack_slots_cap * 2 : 16;
        x64_stack_slot_t *new_slots = realloc(be->stack_slots,
            new_cap * sizeof(x64_stack_slot_t));
        if (!new_slots) return -1;
        be->stack_slots = new_slots;
        be->stack_slots_cap = new_cap;
    }
    
    /* x86-64 stack grows down, allocate 8 bytes per slot */
    be->next_stack_offset += 8;
    int offset = be->next_stack_offset;
    
    be->stack_slots[be->num_stack_slots].value = val;
    be->stack_slots[be->num_stack_slots].offset = offset;
    be->num_stack_slots++;
    
    return offset;
}

/* Get stack slot offset for a value */
static int x64_get_stack_slot(x64_backend_t *be, anvil_value_t *val)
{
    for (size_t i = 0; i < be->num_stack_slots; i++) {
        if (be->stack_slots[i].value == val) {
            return be->stack_slots[i].offset;
        }
    }
    return -1;
}

static const anvil_arch_info_t *x64_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &x64_arch_info;
}

static const char *x64_get_reg_name(int reg, int size)
{
    switch (size) {
        case 1: return x64_gpr8_names[reg];
        case 2: return x64_gpr16_names[reg];
        case 4: return x64_gpr32_names[reg];
        default: return x64_gpr64_names[reg];
    }
}

static const char *x64_size_suffix(int size, anvil_syntax_t syntax)
{
    if (syntax != ANVIL_SYNTAX_GAS) return "";
    switch (size) {
        case 1: return "b";
        case 2: return "w";
        case 4: return "l";
        default: return "q";
    }
}

static void x64_emit_prologue(x64_backend_t *be, anvil_func_t *func, anvil_syntax_t syntax)
{
    if (syntax == ANVIL_SYNTAX_GAS) {
        anvil_strbuf_appendf(&be->code, "\t.globl %s\n", func->name);
        anvil_strbuf_appendf(&be->code, "\t.type %s, @function\n", func->name);
        anvil_strbuf_appendf(&be->code, "%s:\n", func->name);
        anvil_strbuf_append(&be->code, "\tpushq %rbp\n");
        anvil_strbuf_append(&be->code, "\tmovq %rsp, %rbp\n");
        /* Align stack to 16 bytes. After call (8 bytes return addr) + push rbp (8 bytes),
         * RSP is 16-byte aligned again. We need to subtract a multiple of 16 to keep it aligned.
         * But we also need space for local variables. Round up to next multiple of 16. */
        size_t aligned = (func->stack_size + 15) & ~15;
        if (aligned == 0) aligned = 16;  /* Minimum allocation for alignment */
        anvil_strbuf_appendf(&be->code, "\tsubq $%zu, %%rsp\n", aligned);
    } else {
        anvil_strbuf_appendf(&be->code, "global %s\n", func->name);
        anvil_strbuf_appendf(&be->code, "%s:\n", func->name);
        anvil_strbuf_append(&be->code, "\tpush rbp\n");
        anvil_strbuf_append(&be->code, "\tmov rbp, rsp\n");
        size_t aligned = (func->stack_size + 15) & ~15;
        if (aligned == 0) aligned = 16;
        anvil_strbuf_appendf(&be->code, "\tsub rsp, %zu\n", aligned);
    }
}

static void x64_emit_epilogue(x64_backend_t *be, anvil_syntax_t syntax)
{
    if (syntax == ANVIL_SYNTAX_GAS) {
        anvil_strbuf_append(&be->code, "\tmovq %rbp, %rsp\n");
        anvil_strbuf_append(&be->code, "\tpopq %rbp\n");
        anvil_strbuf_append(&be->code, "\tret\n");
    } else {
        anvil_strbuf_append(&be->code, "\tmov rsp, rbp\n");
        anvil_strbuf_append(&be->code, "\tpop rbp\n");
        anvil_strbuf_append(&be->code, "\tret\n");
    }
}

/* Add string to string table and return its label */
static const char *x64_add_string(x64_backend_t *be, const char *str)
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
        x64_string_entry_t *new_strings = realloc(be->strings, 
            new_cap * sizeof(x64_string_entry_t));
        if (!new_strings) return ".str_err";
        be->strings = new_strings;
        be->strings_cap = new_cap;
    }
    
    /* Add new string */
    x64_string_entry_t *entry = &be->strings[be->num_strings];
    entry->str = str;
    entry->len = strlen(str);
    snprintf(entry->label, sizeof(entry->label), ".str%d", be->string_counter++);
    be->num_strings++;
    
    return entry->label;
}

/* Load a value into a register */
static void x64_emit_load_value(x64_backend_t *be, anvil_value_t *val, int target_reg, anvil_syntax_t syntax)
{
    if (!val) return;
    
    const char *reg = x64_gpr64_names[target_reg];
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_INT:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "\tmovq $%lld, %%%s\n", (long long)val->data.i, reg);
            } else {
                anvil_strbuf_appendf(&be->code, "\tmov %s, %lld\n", reg, (long long)val->data.i);
            }
            break;
            
        case ANVIL_VAL_CONST_NULL:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "\txorq %%%s, %%%s\n", reg, reg);
            } else {
                anvil_strbuf_appendf(&be->code, "\txor %s, %s\n", reg, reg);
            }
            break;
            
        case ANVIL_VAL_CONST_STRING:
            {
                const char *label = x64_add_string(be, val->data.str ? val->data.str : "");
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_appendf(&be->code, "\tleaq %s(%%rip), %%%s\n", label, reg);
                } else {
                    anvil_strbuf_appendf(&be->code, "\tlea %s, [rel %s]\n", reg, label);
                }
            }
            break;
            
        case ANVIL_VAL_PARAM:
            if (val->data.param.index < SYSV_NUM_ARG_REGS) {
                int src_reg = sysv_arg_regs[val->data.param.index];
                if (src_reg != target_reg) {
                    if (syntax == ANVIL_SYNTAX_GAS) {
                        anvil_strbuf_appendf(&be->code, "\tmovq %%%s, %%%s\n", x64_gpr64_names[src_reg], reg);
                    } else {
                        anvil_strbuf_appendf(&be->code, "\tmov %s, %s\n", reg, x64_gpr64_names[src_reg]);
                    }
                }
            } else {
                size_t offset = 16 + (val->data.param.index - SYSV_NUM_ARG_REGS) * 8;
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_appendf(&be->code, "\tmovq %zu(%%rbp), %%%s\n", offset, reg);
                } else {
                    anvil_strbuf_appendf(&be->code, "\tmov %s, [rbp+%zu]\n", reg, offset);
                }
            }
            break;
            
        case ANVIL_VAL_INSTR:
            if (val->data.instr && val->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = x64_get_stack_slot(be, val);
                if (offset >= 0) {
                    if (syntax == ANVIL_SYNTAX_GAS) {
                        anvil_strbuf_appendf(&be->code, "\tleaq -%d(%%rbp), %%%s\n", offset, reg);
                    } else {
                        anvil_strbuf_appendf(&be->code, "\tlea %s, [rbp-%d]\n", reg, offset);
                    }
                }
            } else {
                if (target_reg != X64_RAX) {
                    if (syntax == ANVIL_SYNTAX_GAS) {
                        anvil_strbuf_appendf(&be->code, "\tmovq %%rax, %%%s\n", reg);
                    } else {
                        anvil_strbuf_appendf(&be->code, "\tmov %s, rax\n", reg);
                    }
                }
            }
            break;
            
        case ANVIL_VAL_GLOBAL:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "\tleaq %s(%%rip), %%%s\n", val->name, reg);
            } else {
                anvil_strbuf_appendf(&be->code, "\tlea %s, [rel %s]\n", reg, val->name);
            }
            break;
            
        case ANVIL_VAL_FUNC:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "\tleaq %s(%%rip), %%%s\n", val->name, reg);
            } else {
                anvil_strbuf_appendf(&be->code, "\tlea %s, [rel %s]\n", reg, val->name);
            }
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "\t# Unknown value kind %d\n", val->kind);
            break;
    }
}

static void x64_emit_value(x64_backend_t *be, anvil_value_t *val, anvil_syntax_t syntax)
{
    if (!val) return;
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_INT:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "$%lld", (long long)val->data.i);
            } else {
                anvil_strbuf_appendf(&be->code, "%lld", (long long)val->data.i);
            }
            break;
            
        case ANVIL_VAL_CONST_STRING:
            {
                const char *label = x64_add_string(be, val->data.str ? val->data.str : "");
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_appendf(&be->code, "$%s", label);
                } else {
                    anvil_strbuf_appendf(&be->code, "%s", label);
                }
            }
            break;
            
        case ANVIL_VAL_PARAM:
            /* System V ABI: first 6 args in registers, rest on stack */
            if (val->data.param.index < SYSV_NUM_ARG_REGS) {
                int reg = sysv_arg_regs[val->data.param.index];
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_appendf(&be->code, "%%%s", x64_gpr64_names[reg]);
                } else {
                    anvil_strbuf_appendf(&be->code, "%s", x64_gpr64_names[reg]);
                }
            } else {
                /* Stack args at positive offset from RBP */
                size_t offset = 16 + (val->data.param.index - SYSV_NUM_ARG_REGS) * 8;
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_appendf(&be->code, "%zu(%%rbp)", offset);
                } else {
                    anvil_strbuf_appendf(&be->code, "[rbp+%zu]", offset);
                }
            }
            break;
            
        case ANVIL_VAL_INSTR:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "%rax");
            } else {
                anvil_strbuf_append(&be->code, "rax");
            }
            break;
            
        case ANVIL_VAL_GLOBAL:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "%s(%%rip)", val->name);
            } else {
                anvil_strbuf_appendf(&be->code, "[rel %s]", val->name);
            }
            break;
            
        case ANVIL_VAL_FUNC:
            /* Function value - just use the name */
            anvil_strbuf_appendf(&be->code, "%s", val->name);
            break;
            
        default:
            anvil_strbuf_append(&be->code, "???");
            break;
    }
}

static void x64_emit_instr(x64_backend_t *be, anvil_instr_t *instr, anvil_syntax_t syntax)
{
    if (!instr) return;
    
    switch (instr->op) {
        case ANVIL_OP_PHI:
            break;
            
        case ANVIL_OP_ALLOCA:
            {
                int offset = x64_add_stack_slot(be, instr->result);
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_appendf(&be->code, "\tmovq $0, -%d(%%rbp)\n", offset);
                } else {
                    anvil_strbuf_appendf(&be->code, "\tmov qword [rbp-%d], 0\n", offset);
                }
            }
            break;
            
        case ANVIL_OP_ADD:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\taddq %rcx, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tadd rax, rcx\n");
            }
            break;
            
        case ANVIL_OP_SUB:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tsubq %rcx, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tsub rax, rcx\n");
            }
            break;
            
        case ANVIL_OP_MUL:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\timulq %rcx, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\timul rax, rcx\n");
            }
            break;
            
        case ANVIL_OP_SDIV:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tcqo\n\tidivq %rcx\n");
            } else {
                anvil_strbuf_append(&be->code, "\tcqo\n\tidiv rcx\n");
            }
            break;
            
        case ANVIL_OP_UDIV:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\txorq %rdx, %rdx\n\tdivq %rcx\n");
            } else {
                anvil_strbuf_append(&be->code, "\txor rdx, rdx\n\tdiv rcx\n");
            }
            break;
            
        case ANVIL_OP_SMOD:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tcqo\n\tidivq %rcx\n\tmovq %rdx, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tcqo\n\tidiv rcx\n\tmov rax, rdx\n");
            }
            break;
            
        case ANVIL_OP_UMOD:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\txorq %rdx, %rdx\n\tdivq %rcx\n\tmovq %rdx, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\txor rdx, rdx\n\tdiv rcx\n\tmov rax, rdx\n");
            }
            break;
            
        case ANVIL_OP_AND:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tandq %rcx, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tand rax, rcx\n");
            }
            break;
            
        case ANVIL_OP_OR:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\torq %rcx, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tor rax, rcx\n");
            }
            break;
            
        case ANVIL_OP_XOR:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\txorq %rcx, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\txor rax, rcx\n");
            }
            break;
            
        case ANVIL_OP_NOT:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tnotq %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tnot rax\n");
            }
            break;
            
        case ANVIL_OP_NEG:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tnegq %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tneg rax\n");
            }
            break;
            
        case ANVIL_OP_SHL:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tshlq %cl, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov rax, ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tmov rcx, ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n\tshl rax, cl\n");
            }
            break;
            
        case ANVIL_OP_SHR:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tshrq %cl, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tshr rax, cl\n");
            }
            break;
            
        case ANVIL_OP_SAR:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tsarq %cl, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tsar rax, cl\n");
            }
            break;
            
        case ANVIL_OP_LOAD:
            if (instr->operands[0]->kind == ANVIL_VAL_INSTR &&
                instr->operands[0]->data.instr &&
                instr->operands[0]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = x64_get_stack_slot(be, instr->operands[0]);
                if (offset >= 0) {
                    if (syntax == ANVIL_SYNTAX_GAS) {
                        anvil_strbuf_appendf(&be->code, "\tmovq -%d(%%rbp), %%rax\n", offset);
                    } else {
                        anvil_strbuf_appendf(&be->code, "\tmov rax, [rbp-%d]\n", offset);
                    }
                    break;
                }
            }
            if (instr->operands[0]->kind == ANVIL_VAL_GLOBAL) {
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_appendf(&be->code, "\tmovq %s(%%rip), %%rax\n", instr->operands[0]->name);
                } else {
                    anvil_strbuf_appendf(&be->code, "\tmov rax, [rel %s]\n", instr->operands[0]->name);
                }
                break;
            }
            x64_emit_load_value(be, instr->operands[0], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq (%rcx), %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov rax, [rcx]\n");
            }
            break;
            
        case ANVIL_OP_STORE:
            if (instr->operands[1]->kind == ANVIL_VAL_INSTR &&
                instr->operands[1]->data.instr &&
                instr->operands[1]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = x64_get_stack_slot(be, instr->operands[1]);
                if (offset >= 0) {
                    x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
                    if (syntax == ANVIL_SYNTAX_GAS) {
                        anvil_strbuf_appendf(&be->code, "\tmovq %%rax, -%d(%%rbp)\n", offset);
                    } else {
                        anvil_strbuf_appendf(&be->code, "\tmov [rbp-%d], rax\n", offset);
                    }
                    break;
                }
            }
            if (instr->operands[1]->kind == ANVIL_VAL_GLOBAL) {
                x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_appendf(&be->code, "\tmovq %%rax, %s(%%rip)\n", instr->operands[1]->name);
                } else {
                    anvil_strbuf_appendf(&be->code, "\tmov [rel %s], rax\n", instr->operands[1]->name);
                }
                break;
            }
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq %rax, (%rcx)\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov [rcx], rax\n");
            }
            break;
            
        case ANVIL_OP_GEP:
            {
                x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
                if (instr->num_operands > 1) {
                    x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
                    int elem_size = 8;
                    if (instr->result && instr->result->type &&
                        instr->result->type->kind == ANVIL_TYPE_PTR &&
                        instr->result->type->data.pointee) {
                        switch (instr->result->type->data.pointee->kind) {
                            case ANVIL_TYPE_I8: case ANVIL_TYPE_U8: elem_size = 1; break;
                            case ANVIL_TYPE_I16: case ANVIL_TYPE_U16: elem_size = 2; break;
                            case ANVIL_TYPE_I32: case ANVIL_TYPE_U32: case ANVIL_TYPE_F32: elem_size = 4; break;
                            default: elem_size = 8; break;
                        }
                    }
                    if (syntax == ANVIL_SYNTAX_GAS) {
                        if (elem_size == 1) anvil_strbuf_append(&be->code, "\tleaq (%rax,%rcx,1), %rax\n");
                        else if (elem_size == 2) anvil_strbuf_append(&be->code, "\tleaq (%rax,%rcx,2), %rax\n");
                        else if (elem_size == 4) anvil_strbuf_append(&be->code, "\tleaq (%rax,%rcx,4), %rax\n");
                        else if (elem_size == 8) anvil_strbuf_append(&be->code, "\tleaq (%rax,%rcx,8), %rax\n");
                        else { anvil_strbuf_appendf(&be->code, "\timulq $%d, %%rcx\n\taddq %%rcx, %%rax\n", elem_size); }
                    } else {
                        if (elem_size == 1) anvil_strbuf_append(&be->code, "\tlea rax, [rax+rcx*1]\n");
                        else if (elem_size == 2) anvil_strbuf_append(&be->code, "\tlea rax, [rax+rcx*2]\n");
                        else if (elem_size == 4) anvil_strbuf_append(&be->code, "\tlea rax, [rax+rcx*4]\n");
                        else if (elem_size == 8) anvil_strbuf_append(&be->code, "\tlea rax, [rax+rcx*8]\n");
                        else { anvil_strbuf_appendf(&be->code, "\timul rcx, %d\n\tadd rax, rcx\n", elem_size); }
                    }
                }
            }
            break;
            
        case ANVIL_OP_STRUCT_GEP:
            {
                x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
                int offset = 0;
                if (instr->aux_type && instr->aux_type->kind == ANVIL_TYPE_STRUCT &&
                    instr->num_operands > 1 && instr->operands[1]->kind == ANVIL_VAL_CONST_INT) {
                    unsigned idx = (unsigned)instr->operands[1]->data.i;
                    if (idx < instr->aux_type->data.struc.num_fields)
                        offset = (int)instr->aux_type->data.struc.offsets[idx];
                }
                if (offset != 0) {
                    if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_appendf(&be->code, "\taddq $%d, %%rax\n", offset);
                    else anvil_strbuf_appendf(&be->code, "\tadd rax, %d\n", offset);
                }
            }
            break;
            
        case ANVIL_OP_BR:
            if (instr->true_block) {
                anvil_strbuf_appendf(&be->code, "\tjmp .L%s_%s\n", be->current_func->name, instr->true_block->name);
            }
            break;
            
        case ANVIL_OP_BR_COND:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_append(&be->code, "\ttestq %rax, %rax\n");
            else anvil_strbuf_append(&be->code, "\ttest rax, rax\n");
            if (instr->true_block && instr->false_block) {
                anvil_strbuf_appendf(&be->code, "\tjnz .L%s_%s\n", be->current_func->name, instr->true_block->name);
                anvil_strbuf_appendf(&be->code, "\tjmp .L%s_%s\n", be->current_func->name, instr->false_block->name);
            }
            break;
            
        case ANVIL_OP_RET:
            if (instr->num_operands > 0 && instr->operands[0])
                x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_epilogue(be, syntax);
            break;
            
        case ANVIL_OP_CALL:
            /* System V ABI: args in rdi, rsi, rdx, rcx, r8, r9, then stack */
            {
                size_t num_args = instr->num_operands - 1;
                size_t stack_args = num_args > SYSV_NUM_ARG_REGS ? num_args - SYSV_NUM_ARG_REGS : 0;
                
                /* Check if callee is a variadic function */
                bool is_variadic = false;
                anvil_value_t *callee = instr->operands[0];
                if (callee && callee->type && callee->type->kind == ANVIL_TYPE_FUNC) {
                    is_variadic = callee->type->data.func.variadic;
                }
                
                /* Push stack arguments in reverse order */
                for (int i = (int)num_args - 1; i >= SYSV_NUM_ARG_REGS; i--) {
                    if (syntax == ANVIL_SYNTAX_GAS) {
                        anvil_strbuf_append(&be->code, "\tpushq ");
                        x64_emit_value(be, instr->operands[i + 1], syntax);
                        anvil_strbuf_append(&be->code, "\n");
                    } else {
                        anvil_strbuf_append(&be->code, "\tpush ");
                        x64_emit_value(be, instr->operands[i + 1], syntax);
                        anvil_strbuf_append(&be->code, "\n");
                    }
                }
                
                /* Move register arguments */
                for (size_t i = 0; i < num_args && i < SYSV_NUM_ARG_REGS; i++) {
                    int reg = sysv_arg_regs[i];
                    if (syntax == ANVIL_SYNTAX_GAS) {
                        anvil_strbuf_append(&be->code, "\tmovq ");
                        x64_emit_value(be, instr->operands[i + 1], syntax);
                        anvil_strbuf_appendf(&be->code, ", %%%s\n", x64_gpr64_names[reg]);
                    } else {
                        anvil_strbuf_appendf(&be->code, "\tmov %s, ", x64_gpr64_names[reg]);
                        x64_emit_value(be, instr->operands[i + 1], syntax);
                        anvil_strbuf_append(&be->code, "\n");
                    }
                }
                
                /* For variadic functions, %rax must contain the number of vector registers
                 * used for floating-point arguments. Since we don't support FP args yet, set to 0 */
                if (is_variadic) {
                    if (syntax == ANVIL_SYNTAX_GAS) {
                        anvil_strbuf_append(&be->code, "\txorl %eax, %eax\n");
                    } else {
                        anvil_strbuf_append(&be->code, "\txor eax, eax\n");
                    }
                }
                
                /* Call */
                anvil_strbuf_appendf(&be->code, "\tcall %s\n", instr->operands[0]->name);
                
                /* Clean up stack args */
                if (stack_args > 0) {
                    if (syntax == ANVIL_SYNTAX_GAS) {
                        anvil_strbuf_appendf(&be->code, "\taddq $%zu, %%rsp\n", stack_args * 8);
                    } else {
                        anvil_strbuf_appendf(&be->code, "\tadd rsp, %zu\n", stack_args * 8);
                    }
                }
            }
            break;
            
        case ANVIL_OP_CMP_EQ: case ANVIL_OP_CMP_NE: case ANVIL_OP_CMP_LT:
        case ANVIL_OP_CMP_LE: case ANVIL_OP_CMP_GT: case ANVIL_OP_CMP_GE:
            {
                const char *setcc;
                switch (instr->op) {
                    case ANVIL_OP_CMP_EQ: setcc = "sete"; break;
                    case ANVIL_OP_CMP_NE: setcc = "setne"; break;
                    case ANVIL_OP_CMP_LT: setcc = "setl"; break;
                    case ANVIL_OP_CMP_LE: setcc = "setle"; break;
                    case ANVIL_OP_CMP_GT: setcc = "setg"; break;
                    case ANVIL_OP_CMP_GE: setcc = "setge"; break;
                    default: setcc = "sete"; break;
                }
                x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
                x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_append(&be->code, "\tcmpq %rcx, %rax\n");
                    anvil_strbuf_appendf(&be->code, "\t%s %%al\n", setcc);
                    anvil_strbuf_append(&be->code, "\tmovzbq %al, %rax\n");
                } else {
                    anvil_strbuf_append(&be->code, "\tcmp rax, rcx\n");
                    anvil_strbuf_appendf(&be->code, "\t%s al\n", setcc);
                    anvil_strbuf_append(&be->code, "\tmovzx rax, al\n");
                }
            }
            break;
            
        case ANVIL_OP_CMP_ULT: case ANVIL_OP_CMP_ULE: case ANVIL_OP_CMP_UGT: case ANVIL_OP_CMP_UGE:
            {
                const char *setcc;
                switch (instr->op) {
                    case ANVIL_OP_CMP_ULT: setcc = "setb"; break;
                    case ANVIL_OP_CMP_ULE: setcc = "setbe"; break;
                    case ANVIL_OP_CMP_UGT: setcc = "seta"; break;
                    case ANVIL_OP_CMP_UGE: setcc = "setae"; break;
                    default: setcc = "setb"; break;
                }
                x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
                x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_append(&be->code, "\tcmpq %rcx, %rax\n");
                    anvil_strbuf_appendf(&be->code, "\t%s %%al\n", setcc);
                    anvil_strbuf_append(&be->code, "\tmovzbq %al, %rax\n");
                } else {
                    anvil_strbuf_append(&be->code, "\tcmp rax, rcx\n");
                    anvil_strbuf_appendf(&be->code, "\t%s al\n", setcc);
                    anvil_strbuf_append(&be->code, "\tmovzx rax, al\n");
                }
            }
            break;
            
        case ANVIL_OP_TRUNC:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            break;
            
        case ANVIL_OP_ZEXT:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            if (instr->operands[0]->type) {
                if (instr->operands[0]->type->kind == ANVIL_TYPE_I8 || instr->operands[0]->type->kind == ANVIL_TYPE_U8) {
                    if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_append(&be->code, "\tmovzbq %al, %rax\n");
                    else anvil_strbuf_append(&be->code, "\tmovzx rax, al\n");
                } else if (instr->operands[0]->type->kind == ANVIL_TYPE_I16 || instr->operands[0]->type->kind == ANVIL_TYPE_U16) {
                    if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_append(&be->code, "\tmovzwq %ax, %rax\n");
                    else anvil_strbuf_append(&be->code, "\tmovzx rax, ax\n");
                } else if (instr->operands[0]->type->kind == ANVIL_TYPE_I32 || instr->operands[0]->type->kind == ANVIL_TYPE_U32) {
                    if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_append(&be->code, "\tmovl %eax, %eax\n");
                    else anvil_strbuf_append(&be->code, "\tmov eax, eax\n");
                }
            }
            break;
            
        case ANVIL_OP_SEXT:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            if (instr->operands[0]->type) {
                if (instr->operands[0]->type->kind == ANVIL_TYPE_I8) {
                    if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_append(&be->code, "\tmovsbq %al, %rax\n");
                    else anvil_strbuf_append(&be->code, "\tmovsx rax, al\n");
                } else if (instr->operands[0]->type->kind == ANVIL_TYPE_I16) {
                    if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_append(&be->code, "\tmovswq %ax, %rax\n");
                    else anvil_strbuf_append(&be->code, "\tmovsx rax, ax\n");
                } else if (instr->operands[0]->type->kind == ANVIL_TYPE_I32) {
                    if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_append(&be->code, "\tmovslq %eax, %rax\n");
                    else anvil_strbuf_append(&be->code, "\tmovsxd rax, eax\n");
                }
            }
            break;
            
        case ANVIL_OP_BITCAST: case ANVIL_OP_PTRTOINT: case ANVIL_OP_INTTOPTR:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            break;
            
        case ANVIL_OP_SELECT:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            x64_emit_load_value(be, instr->operands[2], X64_RDX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\ttestq %rax, %rax\n\tcmovzq %rdx, %rcx\n\tmovq %rcx, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\ttest rax, rax\n\tcmovz rcx, rdx\n\tmov rax, rcx\n");
            }
            break;
            
        /* Floating-point operations using SSE/SSE2 */
        case ANVIL_OP_FADD:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq %rax, %xmm0\n\tmovq %rcx, %xmm1\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\taddsd %xmm1, %xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\taddss %xmm1, %xmm0\n");
                anvil_strbuf_append(&be->code, "\tmovq %xmm0, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovq xmm0, rax\n\tmovq xmm1, rcx\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\taddsd xmm0, xmm1\n");
                else
                    anvil_strbuf_append(&be->code, "\taddss xmm0, xmm1\n");
                anvil_strbuf_append(&be->code, "\tmovq rax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FSUB:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq %rax, %xmm0\n\tmovq %rcx, %xmm1\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tsubsd %xmm1, %xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\tsubss %xmm1, %xmm0\n");
                anvil_strbuf_append(&be->code, "\tmovq %xmm0, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovq xmm0, rax\n\tmovq xmm1, rcx\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tsubsd xmm0, xmm1\n");
                else
                    anvil_strbuf_append(&be->code, "\tsubss xmm0, xmm1\n");
                anvil_strbuf_append(&be->code, "\tmovq rax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FMUL:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq %rax, %xmm0\n\tmovq %rcx, %xmm1\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tmulsd %xmm1, %xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\tmulss %xmm1, %xmm0\n");
                anvil_strbuf_append(&be->code, "\tmovq %xmm0, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovq xmm0, rax\n\tmovq xmm1, rcx\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tmulsd xmm0, xmm1\n");
                else
                    anvil_strbuf_append(&be->code, "\tmulss xmm0, xmm1\n");
                anvil_strbuf_append(&be->code, "\tmovq rax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FDIV:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq %rax, %xmm0\n\tmovq %rcx, %xmm1\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tdivsd %xmm1, %xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\tdivss %xmm1, %xmm0\n");
                anvil_strbuf_append(&be->code, "\tmovq %xmm0, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovq xmm0, rax\n\tmovq xmm1, rcx\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tdivsd xmm0, xmm1\n");
                else
                    anvil_strbuf_append(&be->code, "\tdivss xmm0, xmm1\n");
                anvil_strbuf_append(&be->code, "\tmovq rax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FNEG:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            if (instr->operands[0]->type && instr->operands[0]->type->kind == ANVIL_TYPE_F64) {
                if (syntax == ANVIL_SYNTAX_GAS)
                    anvil_strbuf_append(&be->code, "\tmovabsq $0x8000000000000000, %rcx\n\txorq %rcx, %rax\n");
                else
                    anvil_strbuf_append(&be->code, "\tmov rcx, 0x8000000000000000\n\txor rax, rcx\n");
            } else {
                if (syntax == ANVIL_SYNTAX_GAS)
                    anvil_strbuf_append(&be->code, "\txorl $0x80000000, %eax\n");
                else
                    anvil_strbuf_append(&be->code, "\txor eax, 0x80000000\n");
            }
            break;
            
        case ANVIL_OP_FABS:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            if (instr->operands[0]->type && instr->operands[0]->type->kind == ANVIL_TYPE_F64) {
                if (syntax == ANVIL_SYNTAX_GAS)
                    anvil_strbuf_append(&be->code, "\tmovabsq $0x7FFFFFFFFFFFFFFF, %rcx\n\tandq %rcx, %rax\n");
                else
                    anvil_strbuf_append(&be->code, "\tmov rcx, 0x7FFFFFFFFFFFFFFF\n\tand rax, rcx\n");
            } else {
                if (syntax == ANVIL_SYNTAX_GAS)
                    anvil_strbuf_append(&be->code, "\tandl $0x7FFFFFFF, %eax\n");
                else
                    anvil_strbuf_append(&be->code, "\tand eax, 0x7FFFFFFF\n");
            }
            break;
            
        case ANVIL_OP_FCMP:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            x64_emit_load_value(be, instr->operands[1], X64_RCX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq %rax, %xmm0\n\tmovq %rcx, %xmm1\n");
                if (instr->operands[0]->type && instr->operands[0]->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tucomisd %xmm1, %xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\tucomiss %xmm1, %xmm0\n");
                anvil_strbuf_append(&be->code, "\tseta %al\n\tmovzbq %al, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovq xmm0, rax\n\tmovq xmm1, rcx\n");
                if (instr->operands[0]->type && instr->operands[0]->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tucomisd xmm0, xmm1\n");
                else
                    anvil_strbuf_append(&be->code, "\tucomiss xmm0, xmm1\n");
                anvil_strbuf_append(&be->code, "\tseta al\n\tmovzx rax, al\n");
            }
            break;
            
        case ANVIL_OP_SITOFP:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvtsi2sdq %rax, %xmm0\n\tmovq %xmm0, %rax\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvtsi2ssq %rax, %xmm0\n\tmovq %xmm0, %rax\n");
            } else {
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvtsi2sd xmm0, rax\n\tmovq rax, xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvtsi2ss xmm0, rax\n\tmovq rax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_UITOFP:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvtsi2sdq %rax, %xmm0\n\tmovq %xmm0, %rax\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvtsi2ssq %rax, %xmm0\n\tmovq %xmm0, %rax\n");
            } else {
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvtsi2sd xmm0, rax\n\tmovq rax, xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvtsi2ss xmm0, rax\n\tmovq rax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FPTOSI:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq %rax, %xmm0\n");
                if (instr->operands[0]->type && instr->operands[0]->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvttsd2siq %xmm0, %rax\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvttss2siq %xmm0, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovq xmm0, rax\n");
                if (instr->operands[0]->type && instr->operands[0]->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvttsd2si rax, xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvttss2si rax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FPTOUI:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq %rax, %xmm0\n");
                if (instr->operands[0]->type && instr->operands[0]->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvttsd2siq %xmm0, %rax\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvttss2siq %xmm0, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovq xmm0, rax\n");
                if (instr->operands[0]->type && instr->operands[0]->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvttsd2si rax, xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvttss2si rax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FPEXT:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovd %eax, %xmm0\n\tcvtss2sd %xmm0, %xmm0\n\tmovq %xmm0, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovd xmm0, eax\n\tcvtss2sd xmm0, xmm0\n\tmovq rax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FPTRUNC:
            x64_emit_load_value(be, instr->operands[0], X64_RAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq %rax, %xmm0\n\tcvtsd2ss %xmm0, %xmm0\n\tmovd %xmm0, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovq xmm0, rax\n\tcvtsd2ss xmm0, xmm0\n\tmovd eax, xmm0\n");
            }
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "\t; unimplemented op %d\n", instr->op);
            break;
    }
}

static void x64_emit_block(x64_backend_t *be, anvil_block_t *block, anvil_syntax_t syntax)
{
    if (!block) return;
    
    /* Emit label with function prefix (skip for entry block) */
    if (block != be->current_func->blocks) {
        anvil_strbuf_appendf(&be->code, ".L%s_%s:\n", be->current_func->name, block->name);
    }
    
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        x64_emit_instr(be, instr, syntax);
    }
}

static void x64_emit_func(x64_backend_t *be, anvil_func_t *func, anvil_syntax_t syntax)
{
    if (!func || func->is_declaration) return;
    
    be->current_func = func;
    be->num_stack_slots = 0;
    be->next_stack_offset = 0;
    
    /* First pass: count stack slots needed */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_ALLOCA) {
                x64_add_stack_slot(be, instr->result);
            }
        }
    }
    
    /* Calculate stack size (16-byte aligned, minimum 32 for shadow space) */
    func->stack_size = (be->next_stack_offset + 32 + 15) & ~15;
    if (func->stack_size < 32) func->stack_size = 32;
    
    x64_emit_prologue(be, func, syntax);
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        x64_emit_block(be, block, syntax);
    }
    
    anvil_strbuf_append(&be->code, "\n");
}

static anvil_error_t x64_codegen_module(anvil_backend_t *be, anvil_module_t *mod,
                                         char **output, size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;
    
    x64_backend_t *priv = be->priv;
    anvil_syntax_t syntax = be->syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS : be->syntax;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    
    /* Reset string table */
    priv->num_strings = 0;
    priv->string_counter = 0;
    
    if (syntax == ANVIL_SYNTAX_GAS) {
        anvil_strbuf_append(&priv->code, "# Generated by ANVIL for x86-64\n");
        anvil_strbuf_append(&priv->code, "\t.text\n");
    } else {
        anvil_strbuf_append(&priv->code, "; Generated by ANVIL for x86-64\n");
        anvil_strbuf_append(&priv->code, "section .text\n");
    }
    
    /* Emit extern declarations for external functions */
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (func->is_declaration) {
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&priv->code, "\t.extern %s\n", func->name);
            } else {
                anvil_strbuf_appendf(&priv->code, "extern %s\n", func->name);
            }
        }
    }
    
    /* Emit function definitions (skip declarations) */
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (!func->is_declaration) {
            x64_emit_func(priv, func, syntax);
        }
    }
    
    if (mod->num_globals > 0 || priv->data.len > 0 || priv->num_strings > 0) {
        if (syntax == ANVIL_SYNTAX_GAS) {
            anvil_strbuf_append(&priv->code, "\t.data\n");
        } else {
            anvil_strbuf_append(&priv->code, "section .data\n");
        }
        if (priv->data.data) {
            anvil_strbuf_append(&priv->code, priv->data.data);
        }
        
        /* Emit string constants */
        for (size_t i = 0; i < priv->num_strings; i++) {
            x64_string_entry_t *entry = &priv->strings[i];
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&priv->code, "%s:\n", entry->label);
                anvil_strbuf_appendf(&priv->code, "\t.asciz \"");
            } else {
                anvil_strbuf_appendf(&priv->code, "%s:\n", entry->label);
                anvil_strbuf_appendf(&priv->code, "\tdb \"");
            }
            /* Escape special characters */
            for (const char *p = entry->str; *p; p++) {
                if (*p == '"') {
                    anvil_strbuf_append(&priv->code, "\\\"");
                } else if (*p == '\\') {
                    anvil_strbuf_append(&priv->code, "\\\\");
                } else if (*p == '\n') {
                    anvil_strbuf_append(&priv->code, "\\n");
                } else if (*p == '\t') {
                    anvil_strbuf_append(&priv->code, "\\t");
                } else {
                    anvil_strbuf_append_char(&priv->code, *p);
                }
            }
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&priv->code, "\"\n");
            } else {
                anvil_strbuf_append(&priv->code, "\", 0\n");
            }
        }
    }
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

static anvil_error_t x64_codegen_func(anvil_backend_t *be, anvil_func_t *func,
                                       char **output, size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    
    x64_backend_t *priv = be->priv;
    anvil_syntax_t syntax = be->syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS : be->syntax;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);
    
    x64_emit_func(priv, func, syntax);
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

const anvil_backend_ops_t anvil_backend_x86_64 = {
    .name = "x86-64",
    .arch = ANVIL_ARCH_X86_64,
    .init = x64_init,
    .cleanup = x64_cleanup,
    .codegen_module = x64_codegen_module,
    .codegen_func = x64_codegen_func,
    .get_arch_info = x64_get_arch_info
};
