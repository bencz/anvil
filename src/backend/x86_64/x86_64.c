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

/* Backend private data */
typedef struct {
    anvil_strbuf_t code;
    anvil_strbuf_t data;
    anvil_syntax_t syntax;
    int label_counter;
    int string_counter;
    int stack_offset;
    
    /* String table */
    x64_string_entry_t *strings;
    size_t num_strings;
    size_t strings_cap;
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
    free(priv);
    be->priv = NULL;
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
        case ANVIL_OP_ADD:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
                anvil_strbuf_append(&be->code, "\taddq ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov rax, ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tadd rax, ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n");
            }
            break;
            
        case ANVIL_OP_SUB:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
                anvil_strbuf_append(&be->code, "\tsubq ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov rax, ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tsub rax, ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n");
            }
            break;
            
        case ANVIL_OP_MUL:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
                anvil_strbuf_append(&be->code, "\timulq ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov rax, ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\timul rax, ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n");
            }
            break;
            
        case ANVIL_OP_SDIV:
        case ANVIL_OP_UDIV:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
                anvil_strbuf_append(&be->code, "\tcqo\n");
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %rcx\n");
                anvil_strbuf_appendf(&be->code, "\t%s %%rcx\n",
                    instr->op == ANVIL_OP_SDIV ? "idivq" : "divq");
            } else {
                anvil_strbuf_append(&be->code, "\tmov rax, ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tcqo\n\tmov rcx, ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_appendf(&be->code, "\n\t%s rcx\n",
                    instr->op == ANVIL_OP_SDIV ? "idiv" : "div");
            }
            break;
            
        case ANVIL_OP_AND:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
                anvil_strbuf_append(&be->code, "\tandq ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov rax, ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tand rax, ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n");
            }
            break;
            
        case ANVIL_OP_OR:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
                anvil_strbuf_append(&be->code, "\torq ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov rax, ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tor rax, ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n");
            }
            break;
            
        case ANVIL_OP_XOR:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
                anvil_strbuf_append(&be->code, "\txorq ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov rax, ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\txor rax, ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n");
            }
            break;
            
        case ANVIL_OP_SHL:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %rcx\n");
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
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %rcx\n");
                anvil_strbuf_append(&be->code, "\tshrq %cl, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov rax, ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tmov rcx, ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n\tshr rax, cl\n");
            }
            break;
            
        case ANVIL_OP_SAR:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %rcx\n");
                anvil_strbuf_append(&be->code, "\tsarq %cl, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov rax, ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tmov rcx, ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n\tsar rax, cl\n");
            }
            break;
            
        case ANVIL_OP_LOAD:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
                anvil_strbuf_append(&be->code, "\tmovq (%rax), %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov rax, ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tmov rax, [rax]\n");
            }
            break;
            
        case ANVIL_OP_STORE:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %rcx\n");
                anvil_strbuf_append(&be->code, "\tmovq %rax, (%rcx)\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov rax, ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tmov rcx, ");
                x64_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n\tmov [rcx], rax\n");
            }
            break;
            
        case ANVIL_OP_BR:
            anvil_strbuf_appendf(&be->code, "\tjmp .L%s\n", instr->true_block->name);
            break;
            
        case ANVIL_OP_BR_COND:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %rax\n");
                anvil_strbuf_append(&be->code, "\ttestq %rax, %rax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov rax, ");
                x64_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\ttest rax, rax\n");
            }
            anvil_strbuf_appendf(&be->code, "\tjnz .L%s\n", instr->true_block->name);
            anvil_strbuf_appendf(&be->code, "\tjmp .L%s\n", instr->false_block->name);
            break;
            
        case ANVIL_OP_RET:
            if (instr->num_operands > 0) {
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_append(&be->code, "\tmovq ");
                    x64_emit_value(be, instr->operands[0], syntax);
                    anvil_strbuf_append(&be->code, ", %rax\n");
                } else {
                    anvil_strbuf_append(&be->code, "\tmov rax, ");
                    x64_emit_value(be, instr->operands[0], syntax);
                    anvil_strbuf_append(&be->code, "\n");
                }
            }
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
            
        case ANVIL_OP_CMP_EQ:
        case ANVIL_OP_CMP_NE:
        case ANVIL_OP_CMP_LT:
        case ANVIL_OP_CMP_LE:
        case ANVIL_OP_CMP_GT:
        case ANVIL_OP_CMP_GE:
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
                
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_append(&be->code, "\tmovq ");
                    x64_emit_value(be, instr->operands[0], syntax);
                    anvil_strbuf_append(&be->code, ", %rax\n");
                    anvil_strbuf_append(&be->code, "\tcmpq ");
                    x64_emit_value(be, instr->operands[1], syntax);
                    anvil_strbuf_append(&be->code, ", %rax\n");
                    anvil_strbuf_appendf(&be->code, "\t%s %%al\n", setcc);
                    anvil_strbuf_append(&be->code, "\tmovzbq %al, %rax\n");
                } else {
                    anvil_strbuf_append(&be->code, "\tmov rax, ");
                    x64_emit_value(be, instr->operands[0], syntax);
                    anvil_strbuf_append(&be->code, "\n\tcmp rax, ");
                    x64_emit_value(be, instr->operands[1], syntax);
                    anvil_strbuf_appendf(&be->code, "\n\t%s al\n", setcc);
                    anvil_strbuf_append(&be->code, "\tmovzx rax, al\n");
                }
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
    
    anvil_strbuf_appendf(&be->code, ".L%s:\n", block->name);
    
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        x64_emit_instr(be, instr, syntax);
    }
}

static void x64_emit_func(x64_backend_t *be, anvil_func_t *func, anvil_syntax_t syntax)
{
    if (!func) return;
    
    func->stack_size = 32; /* Shadow space + alignment */
    
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
