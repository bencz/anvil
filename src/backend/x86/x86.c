/*
 * ANVIL - x86 (32-bit) Backend
 * 
 * Little-endian, stack grows downward
 * Generates GAS or NASM syntax
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* x86 registers */
static const char *x86_gpr_names[] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"
};

static const char *x86_gpr8_names[] = {
    "al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"
};

static const char *x86_gpr16_names[] = {
    "ax", "cx", "dx", "bx", "sp", "bp", "si", "di"
};

/* Register indices */
#define X86_EAX 0
#define X86_ECX 1
#define X86_EDX 2
#define X86_EBX 3
#define X86_ESP 4
#define X86_EBP 5
#define X86_ESI 6
#define X86_EDI 7

/* String table entry */
typedef struct {
    const char *str;
    char label[16];
    size_t len;
} x86_string_entry_t;

/* Backend private data */
typedef struct {
    anvil_strbuf_t code;
    anvil_strbuf_t data;
    anvil_syntax_t syntax;
    int label_counter;
    int string_counter;
    int stack_offset;
    
    /* String table */
    x86_string_entry_t *strings;
    size_t num_strings;
    size_t strings_cap;
} x86_backend_t;

static const anvil_arch_info_t x86_arch_info = {
    .arch = ANVIL_ARCH_X86,
    .name = "x86",
    .ptr_size = 4,
    .addr_bits = 32,
    .word_size = 4,
    .num_gpr = 8,
    .num_fpr = 8,
    .endian = ANVIL_ENDIAN_LITTLE,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false
};

/* Forward declarations */
static void x86_emit_func(x86_backend_t *be, anvil_func_t *func, anvil_syntax_t syntax);
static void x86_emit_instr(x86_backend_t *be, anvil_instr_t *instr, anvil_syntax_t syntax);
static const char *x86_get_reg(anvil_type_t *type, int reg);

static anvil_error_t x86_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    x86_backend_t *priv = calloc(1, sizeof(x86_backend_t));
    if (!priv) return ANVIL_ERR_NOMEM;
    
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->syntax = ctx->syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS : ctx->syntax;
    
    be->priv = priv;
    return ANVIL_OK;
}

static void x86_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    x86_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv->strings);
    free(priv);
    be->priv = NULL;
}

static const anvil_arch_info_t *x86_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &x86_arch_info;
}

static void x86_emit_prologue(x86_backend_t *be, anvil_func_t *func, anvil_syntax_t syntax)
{
    if (syntax == ANVIL_SYNTAX_GAS) {
        anvil_strbuf_appendf(&be->code, "\t.globl %s\n", func->name);
        anvil_strbuf_appendf(&be->code, "\t.type %s, @function\n", func->name);
        anvil_strbuf_appendf(&be->code, "%s:\n", func->name);
        anvil_strbuf_append(&be->code, "\tpushl %ebp\n");
        anvil_strbuf_append(&be->code, "\tmovl %esp, %ebp\n");
        if (func->stack_size > 0) {
            anvil_strbuf_appendf(&be->code, "\tsubl $%zu, %%esp\n", func->stack_size);
        }
    } else { /* NASM */
        anvil_strbuf_appendf(&be->code, "global %s\n", func->name);
        anvil_strbuf_appendf(&be->code, "%s:\n", func->name);
        anvil_strbuf_append(&be->code, "\tpush ebp\n");
        anvil_strbuf_append(&be->code, "\tmov ebp, esp\n");
        if (func->stack_size > 0) {
            anvil_strbuf_appendf(&be->code, "\tsub esp, %zu\n", func->stack_size);
        }
    }
}

static void x86_emit_epilogue(x86_backend_t *be, anvil_syntax_t syntax)
{
    if (syntax == ANVIL_SYNTAX_GAS) {
        anvil_strbuf_append(&be->code, "\tmovl %ebp, %esp\n");
        anvil_strbuf_append(&be->code, "\tpopl %ebp\n");
        anvil_strbuf_append(&be->code, "\tret\n");
    } else {
        anvil_strbuf_append(&be->code, "\tmov esp, ebp\n");
        anvil_strbuf_append(&be->code, "\tpop ebp\n");
        anvil_strbuf_append(&be->code, "\tret\n");
    }
}

static const char *x86_get_reg(anvil_type_t *type, int reg)
{
    if (!type) return x86_gpr_names[reg];
    
    switch (type->size) {
        case 1: return x86_gpr8_names[reg];
        case 2: return x86_gpr16_names[reg];
        default: return x86_gpr_names[reg];
    }
}

/* Add string to string table and return its label */
static const char *x86_add_string(x86_backend_t *be, const char *str)
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
        x86_string_entry_t *new_strings = realloc(be->strings, 
            new_cap * sizeof(x86_string_entry_t));
        if (!new_strings) return ".str_err";
        be->strings = new_strings;
        be->strings_cap = new_cap;
    }
    
    /* Add new string */
    x86_string_entry_t *entry = &be->strings[be->num_strings];
    entry->str = str;
    entry->len = strlen(str);
    snprintf(entry->label, sizeof(entry->label), ".str%d", be->string_counter++);
    be->num_strings++;
    
    return entry->label;
}

static void x86_emit_value(x86_backend_t *be, anvil_value_t *val, anvil_syntax_t syntax)
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
                const char *label = x86_add_string(be, val->data.str ? val->data.str : "");
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_appendf(&be->code, "$%s", label);
                } else {
                    anvil_strbuf_appendf(&be->code, "%s", label);
                }
            }
            break;
            
        case ANVIL_VAL_PARAM:
            /* Parameters are at positive offsets from EBP */
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "%zu(%%ebp)", 8 + val->data.param.index * 4);
            } else {
                anvil_strbuf_appendf(&be->code, "[ebp+%zu]", 8 + val->data.param.index * 4);
            }
            break;
            
        case ANVIL_VAL_INSTR:
            /* Instruction results - simplified, using EAX */
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "%%eax");
            } else {
                anvil_strbuf_appendf(&be->code, "eax");
            }
            break;
            
        case ANVIL_VAL_GLOBAL:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "%s", val->name);
            } else {
                anvil_strbuf_appendf(&be->code, "[%s]", val->name);
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

static void x86_emit_instr(x86_backend_t *be, anvil_instr_t *instr, anvil_syntax_t syntax)
{
    if (!instr) return;
    
    switch (instr->op) {
        case ANVIL_OP_ADD:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
                anvil_strbuf_append(&be->code, "\taddl ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov eax, ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tadd eax, ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n");
            }
            break;
            
        case ANVIL_OP_SUB:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
                anvil_strbuf_append(&be->code, "\tsubl ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov eax, ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tsub eax, ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n");
            }
            break;
            
        case ANVIL_OP_MUL:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
                anvil_strbuf_append(&be->code, "\timull ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov eax, ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\timul eax, ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n");
            }
            break;
            
        case ANVIL_OP_SDIV:
        case ANVIL_OP_UDIV:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
                anvil_strbuf_append(&be->code, "\tcdq\n");
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %ecx\n");
                anvil_strbuf_appendf(&be->code, "\t%s %%ecx\n", 
                    instr->op == ANVIL_OP_SDIV ? "idivl" : "divl");
            } else {
                anvil_strbuf_append(&be->code, "\tmov eax, ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tcdq\n\tmov ecx, ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_appendf(&be->code, "\n\t%s ecx\n",
                    instr->op == ANVIL_OP_SDIV ? "idiv" : "div");
            }
            break;
            
        case ANVIL_OP_AND:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
                anvil_strbuf_append(&be->code, "\tandl ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov eax, ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tand eax, ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n");
            }
            break;
            
        case ANVIL_OP_OR:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
                anvil_strbuf_append(&be->code, "\torl ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov eax, ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tor eax, ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n");
            }
            break;
            
        case ANVIL_OP_XOR:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
                anvil_strbuf_append(&be->code, "\txorl ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov eax, ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\txor eax, ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n");
            }
            break;
            
        case ANVIL_OP_SHL:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %ecx\n");
                anvil_strbuf_append(&be->code, "\tshll %cl, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov eax, ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tmov ecx, ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n\tshl eax, cl\n");
            }
            break;
            
        case ANVIL_OP_SHR:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %ecx\n");
                anvil_strbuf_append(&be->code, "\tshrl %cl, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov eax, ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tmov ecx, ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n\tshr eax, cl\n");
            }
            break;
            
        case ANVIL_OP_SAR:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %ecx\n");
                anvil_strbuf_append(&be->code, "\tsarl %cl, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov eax, ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tmov ecx, ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n\tsar eax, cl\n");
            }
            break;
            
        case ANVIL_OP_LOAD:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
                anvil_strbuf_append(&be->code, "\tmovl (%eax), %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov eax, ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tmov eax, [eax]\n");
            }
            break;
            
        case ANVIL_OP_STORE:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, ", %ecx\n");
                anvil_strbuf_append(&be->code, "\tmovl %eax, (%ecx)\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov eax, ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\tmov ecx, ");
                x86_emit_value(be, instr->operands[1], syntax);
                anvil_strbuf_append(&be->code, "\n\tmov [ecx], eax\n");
            }
            break;
            
        case ANVIL_OP_BR:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "\tjmp .L%s\n", instr->true_block->name);
            } else {
                anvil_strbuf_appendf(&be->code, "\tjmp .L%s\n", instr->true_block->name);
            }
            break;
            
        case ANVIL_OP_BR_COND:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, ", %eax\n");
                anvil_strbuf_append(&be->code, "\ttestl %eax, %eax\n");
                anvil_strbuf_appendf(&be->code, "\tjnz .L%s\n", instr->true_block->name);
                anvil_strbuf_appendf(&be->code, "\tjmp .L%s\n", instr->false_block->name);
            } else {
                anvil_strbuf_append(&be->code, "\tmov eax, ");
                x86_emit_value(be, instr->operands[0], syntax);
                anvil_strbuf_append(&be->code, "\n\ttest eax, eax\n");
                anvil_strbuf_appendf(&be->code, "\tjnz .L%s\n", instr->true_block->name);
                anvil_strbuf_appendf(&be->code, "\tjmp .L%s\n", instr->false_block->name);
            }
            break;
            
        case ANVIL_OP_RET:
            if (instr->num_operands > 0) {
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_append(&be->code, "\tmovl ");
                    x86_emit_value(be, instr->operands[0], syntax);
                    anvil_strbuf_append(&be->code, ", %eax\n");
                } else {
                    anvil_strbuf_append(&be->code, "\tmov eax, ");
                    x86_emit_value(be, instr->operands[0], syntax);
                    anvil_strbuf_append(&be->code, "\n");
                }
            }
            x86_emit_epilogue(be, syntax);
            break;
            
        case ANVIL_OP_CALL:
            /* Push arguments in reverse order (cdecl) */
            for (int i = (int)instr->num_operands - 1; i >= 1; i--) {
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_append(&be->code, "\tpushl ");
                    x86_emit_value(be, instr->operands[i], syntax);
                    anvil_strbuf_append(&be->code, "\n");
                } else {
                    anvil_strbuf_append(&be->code, "\tpush ");
                    x86_emit_value(be, instr->operands[i], syntax);
                    anvil_strbuf_append(&be->code, "\n");
                }
            }
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "\tcall %s\n", instr->operands[0]->name);
                if (instr->num_operands > 1) {
                    anvil_strbuf_appendf(&be->code, "\taddl $%zu, %%esp\n", 
                        (instr->num_operands - 1) * 4);
                }
            } else {
                anvil_strbuf_appendf(&be->code, "\tcall %s\n", instr->operands[0]->name);
                if (instr->num_operands > 1) {
                    anvil_strbuf_appendf(&be->code, "\tadd esp, %zu\n",
                        (instr->num_operands - 1) * 4);
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
                    anvil_strbuf_append(&be->code, "\tmovl ");
                    x86_emit_value(be, instr->operands[0], syntax);
                    anvil_strbuf_append(&be->code, ", %eax\n");
                    anvil_strbuf_append(&be->code, "\tcmpl ");
                    x86_emit_value(be, instr->operands[1], syntax);
                    anvil_strbuf_append(&be->code, ", %eax\n");
                    anvil_strbuf_appendf(&be->code, "\t%s %%al\n", setcc);
                    anvil_strbuf_append(&be->code, "\tmovzbl %al, %eax\n");
                } else {
                    anvil_strbuf_append(&be->code, "\tmov eax, ");
                    x86_emit_value(be, instr->operands[0], syntax);
                    anvil_strbuf_append(&be->code, "\n\tcmp eax, ");
                    x86_emit_value(be, instr->operands[1], syntax);
                    anvil_strbuf_appendf(&be->code, "\n\t%s al\n", setcc);
                    anvil_strbuf_append(&be->code, "\tmovzx eax, al\n");
                }
            }
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "\t; unimplemented op %d\n", instr->op);
            break;
    }
}

static void x86_emit_block(x86_backend_t *be, anvil_block_t *block, anvil_syntax_t syntax)
{
    if (!block) return;
    
    /* Emit label */
    anvil_strbuf_appendf(&be->code, ".L%s:\n", block->name);
    
    /* Emit instructions */
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        x86_emit_instr(be, instr, syntax);
    }
}

static void x86_emit_func(x86_backend_t *be, anvil_func_t *func, anvil_syntax_t syntax)
{
    if (!func) return;
    
    /* Calculate stack size for locals */
    func->stack_size = 16; /* Minimum for alignment */
    
    /* Emit prologue */
    x86_emit_prologue(be, func, syntax);
    
    /* Emit blocks */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        x86_emit_block(be, block, syntax);
    }
    
    anvil_strbuf_append(&be->code, "\n");
}

static anvil_error_t x86_codegen_module(anvil_backend_t *be, anvil_module_t *mod,
                                         char **output, size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;
    
    x86_backend_t *priv = be->priv;
    anvil_syntax_t syntax = be->syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS : be->syntax;
    
    /* Reset buffers */
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    
    /* Reset string table */
    priv->num_strings = 0;
    priv->string_counter = 0;
    
    /* Emit header */
    if (syntax == ANVIL_SYNTAX_GAS) {
        anvil_strbuf_appendf(&priv->code, "# Generated by ANVIL for x86\n");
        anvil_strbuf_append(&priv->code, "\t.text\n");
    } else {
        anvil_strbuf_appendf(&priv->code, "; Generated by ANVIL for x86\n");
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
            x86_emit_func(priv, func, syntax);
        }
    }
    
    /* Emit data section if needed */
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
            x86_string_entry_t *entry = &priv->strings[i];
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

static anvil_error_t x86_codegen_func(anvil_backend_t *be, anvil_func_t *func,
                                       char **output, size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    
    x86_backend_t *priv = be->priv;
    anvil_syntax_t syntax = be->syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS : be->syntax;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);
    
    x86_emit_func(priv, func, syntax);
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

const anvil_backend_ops_t anvil_backend_x86 = {
    .name = "x86",
    .arch = ANVIL_ARCH_X86,
    .init = x86_init,
    .cleanup = x86_cleanup,
    .codegen_module = x86_codegen_module,
    .codegen_func = x86_codegen_func,
    .get_arch_info = x86_get_arch_info
};
