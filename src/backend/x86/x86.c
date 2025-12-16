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

/* Stack slot for local variables */
typedef struct {
    anvil_value_t *value;
    int offset;
} x86_stack_slot_t;

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
    x86_stack_slot_t *stack_slots;
    size_t num_stack_slots;
    size_t stack_slots_cap;
    
    /* String table */
    x86_string_entry_t *strings;
    size_t num_strings;
    size_t strings_cap;
    
    /* Current function being generated */
    anvil_func_t *current_func;
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
    free(priv->stack_slots);
    free(priv);
    be->priv = NULL;
}

static void x86_reset(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    x86_backend_t *priv = be->priv;
    
    /* Clear stack slots (contain pointers to anvil_value_t) */
    priv->num_stack_slots = 0;
    priv->next_stack_offset = 0;
    priv->stack_offset = 0;
    
    /* Clear string table (contain pointers to string data) */
    priv->num_strings = 0;
    priv->string_counter = 0;
    
    /* Reset other state */
    priv->label_counter = 0;
    priv->current_func = NULL;
}

/* Add stack slot for local variable */
static int x86_add_stack_slot(x86_backend_t *be, anvil_value_t *val)
{
    if (be->num_stack_slots >= be->stack_slots_cap) {
        size_t new_cap = be->stack_slots_cap ? be->stack_slots_cap * 2 : 16;
        x86_stack_slot_t *new_slots = realloc(be->stack_slots,
            new_cap * sizeof(x86_stack_slot_t));
        if (!new_slots) return -1;
        be->stack_slots = new_slots;
        be->stack_slots_cap = new_cap;
    }
    
    /* x86 stack grows down, allocate 4 bytes per slot */
    be->next_stack_offset += 4;
    int offset = be->next_stack_offset;
    
    be->stack_slots[be->num_stack_slots].value = val;
    be->stack_slots[be->num_stack_slots].offset = offset;
    be->num_stack_slots++;
    
    return offset;
}

/* Get stack slot offset for a value */
static int x86_get_stack_slot(x86_backend_t *be, anvil_value_t *val)
{
    for (size_t i = 0; i < be->num_stack_slots; i++) {
        if (be->stack_slots[i].value == val) {
            return be->stack_slots[i].offset;
        }
    }
    return -1;
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

/* Load a value into a register */
static void x86_emit_load_value(x86_backend_t *be, anvil_value_t *val, int target_reg, anvil_syntax_t syntax)
{
    if (!val) return;
    
    const char *reg = x86_gpr_names[target_reg];
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_INT:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "\tmovl $%lld, %%%s\n", (long long)val->data.i, reg);
            } else {
                anvil_strbuf_appendf(&be->code, "\tmov %s, %lld\n", reg, (long long)val->data.i);
            }
            break;
            
        case ANVIL_VAL_CONST_NULL:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "\txorl %%%s, %%%s\n", reg, reg);
            } else {
                anvil_strbuf_appendf(&be->code, "\txor %s, %s\n", reg, reg);
            }
            break;
            
        case ANVIL_VAL_CONST_STRING:
            {
                const char *label = x86_add_string(be, val->data.str ? val->data.str : "");
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_appendf(&be->code, "\tmovl $%s, %%%s\n", label, reg);
                } else {
                    anvil_strbuf_appendf(&be->code, "\tmov %s, %s\n", reg, label);
                }
            }
            break;
            
        case ANVIL_VAL_PARAM:
            /* Parameters are at positive offsets from EBP (cdecl: return addr + saved ebp = 8) */
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "\tmovl %zu(%%ebp), %%%s\n", 8 + val->data.param.index * 4, reg);
            } else {
                anvil_strbuf_appendf(&be->code, "\tmov %s, [ebp+%zu]\n", reg, 8 + val->data.param.index * 4);
            }
            break;
            
        case ANVIL_VAL_INSTR:
            if (val->data.instr && val->data.instr->op == ANVIL_OP_ALLOCA) {
                /* Load address of stack slot */
                int offset = x86_get_stack_slot(be, val);
                if (offset >= 0) {
                    if (syntax == ANVIL_SYNTAX_GAS) {
                        anvil_strbuf_appendf(&be->code, "\tleal -%d(%%ebp), %%%s\n", offset, reg);
                    } else {
                        anvil_strbuf_appendf(&be->code, "\tlea %s, [ebp-%d]\n", reg, offset);
                    }
                }
            } else {
                /* Result should be in EAX by convention */
                if (target_reg != X86_EAX) {
                    if (syntax == ANVIL_SYNTAX_GAS) {
                        anvil_strbuf_appendf(&be->code, "\tmovl %%eax, %%%s\n", reg);
                    } else {
                        anvil_strbuf_appendf(&be->code, "\tmov %s, eax\n", reg);
                    }
                }
            }
            break;
            
        case ANVIL_VAL_GLOBAL:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "\tmovl $%s, %%%s\n", val->name, reg);
            } else {
                anvil_strbuf_appendf(&be->code, "\tmov %s, %s\n", reg, val->name);
            }
            break;
            
        case ANVIL_VAL_FUNC:
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_appendf(&be->code, "\tmovl $%s, %%%s\n", val->name, reg);
            } else {
                anvil_strbuf_appendf(&be->code, "\tmov %s, %s\n", reg, val->name);
            }
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "\t# Unknown value kind %d\n", val->kind);
            break;
    }
}

/* Legacy emit_value for backward compatibility */
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
        case ANVIL_OP_PHI:
            break;
            
        case ANVIL_OP_ALLOCA:
            {
                int offset = x86_add_stack_slot(be, instr->result);
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_appendf(&be->code, "\tmovl $0, -%d(%%ebp)\n", offset);
                } else {
                    anvil_strbuf_appendf(&be->code, "\tmov dword [ebp-%d], 0\n", offset);
                }
            }
            break;
            
        case ANVIL_OP_ADD:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\taddl %ecx, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tadd eax, ecx\n");
            }
            break;
            
        case ANVIL_OP_SUB:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tsubl %ecx, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tsub eax, ecx\n");
            }
            break;
            
        case ANVIL_OP_MUL:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\timull %ecx, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\timul eax, ecx\n");
            }
            break;
            
        case ANVIL_OP_SDIV:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tcdq\n\tidivl %ecx\n");
            } else {
                anvil_strbuf_append(&be->code, "\tcdq\n\tidiv ecx\n");
            }
            break;
            
        case ANVIL_OP_UDIV:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\txorl %edx, %edx\n\tdivl %ecx\n");
            } else {
                anvil_strbuf_append(&be->code, "\txor edx, edx\n\tdiv ecx\n");
            }
            break;
            
        case ANVIL_OP_SMOD:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tcdq\n\tidivl %ecx\n\tmovl %edx, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tcdq\n\tidiv ecx\n\tmov eax, edx\n");
            }
            break;
            
        case ANVIL_OP_UMOD:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\txorl %edx, %edx\n\tdivl %ecx\n\tmovl %edx, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\txor edx, edx\n\tdiv ecx\n\tmov eax, edx\n");
            }
            break;
            
        case ANVIL_OP_AND:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tandl %ecx, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tand eax, ecx\n");
            }
            break;
            
        case ANVIL_OP_OR:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\torl %ecx, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tor eax, ecx\n");
            }
            break;
            
        case ANVIL_OP_XOR:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\txorl %ecx, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\txor eax, ecx\n");
            }
            break;
            
        case ANVIL_OP_NOT:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tnotl %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tnot eax\n");
            }
            break;
            
        case ANVIL_OP_NEG:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tnegl %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tneg eax\n");
            }
            break;
            
        case ANVIL_OP_SHL:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tshll %cl, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tshl eax, cl\n");
            }
            break;
            
        case ANVIL_OP_SHR:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tshrl %cl, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tshr eax, cl\n");
            }
            break;
            
        case ANVIL_OP_SAR:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tsarl %cl, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tsar eax, cl\n");
            }
            break;
            
        case ANVIL_OP_LOAD:
            if (instr->operands[0]->kind == ANVIL_VAL_INSTR &&
                instr->operands[0]->data.instr &&
                instr->operands[0]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = x86_get_stack_slot(be, instr->operands[0]);
                if (offset >= 0) {
                    if (syntax == ANVIL_SYNTAX_GAS) {
                        anvil_strbuf_appendf(&be->code, "\tmovl -%d(%%ebp), %%eax\n", offset);
                    } else {
                        anvil_strbuf_appendf(&be->code, "\tmov eax, [ebp-%d]\n", offset);
                    }
                    break;
                }
            }
            if (instr->operands[0]->kind == ANVIL_VAL_GLOBAL) {
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_appendf(&be->code, "\tmovl %s, %%eax\n", instr->operands[0]->name);
                } else {
                    anvil_strbuf_appendf(&be->code, "\tmov eax, [%s]\n", instr->operands[0]->name);
                }
                break;
            }
            x86_emit_load_value(be, instr->operands[0], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl (%ecx), %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov eax, [ecx]\n");
            }
            break;
            
        case ANVIL_OP_STORE:
            if (instr->operands[1]->kind == ANVIL_VAL_INSTR &&
                instr->operands[1]->data.instr &&
                instr->operands[1]->data.instr->op == ANVIL_OP_ALLOCA) {
                int offset = x86_get_stack_slot(be, instr->operands[1]);
                if (offset >= 0) {
                    x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
                    if (syntax == ANVIL_SYNTAX_GAS) {
                        anvil_strbuf_appendf(&be->code, "\tmovl %%eax, -%d(%%ebp)\n", offset);
                    } else {
                        anvil_strbuf_appendf(&be->code, "\tmov [ebp-%d], eax\n", offset);
                    }
                    break;
                }
            }
            if (instr->operands[1]->kind == ANVIL_VAL_GLOBAL) {
                x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_appendf(&be->code, "\tmovl %%eax, %s\n", instr->operands[1]->name);
                } else {
                    anvil_strbuf_appendf(&be->code, "\tmov [%s], eax\n", instr->operands[1]->name);
                }
                break;
            }
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl %eax, (%ecx)\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov [ecx], eax\n");
            }
            break;
            
        case ANVIL_OP_GEP:
            {
                x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
                if (instr->num_operands > 1) {
                    x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
                    int elem_size = 4;
                    if (instr->result && instr->result->type &&
                        instr->result->type->kind == ANVIL_TYPE_PTR &&
                        instr->result->type->data.pointee) {
                        switch (instr->result->type->data.pointee->kind) {
                            case ANVIL_TYPE_I8: case ANVIL_TYPE_U8: elem_size = 1; break;
                            case ANVIL_TYPE_I16: case ANVIL_TYPE_U16: elem_size = 2; break;
                            case ANVIL_TYPE_I64: case ANVIL_TYPE_U64: case ANVIL_TYPE_F64: elem_size = 8; break;
                            default: elem_size = 4; break;
                        }
                    }
                    if (syntax == ANVIL_SYNTAX_GAS) {
                        if (elem_size == 1) anvil_strbuf_append(&be->code, "\tleal (%eax,%ecx,1), %eax\n");
                        else if (elem_size == 2) anvil_strbuf_append(&be->code, "\tleal (%eax,%ecx,2), %eax\n");
                        else if (elem_size == 4) anvil_strbuf_append(&be->code, "\tleal (%eax,%ecx,4), %eax\n");
                        else if (elem_size == 8) anvil_strbuf_append(&be->code, "\tleal (%eax,%ecx,8), %eax\n");
                        else { anvil_strbuf_appendf(&be->code, "\timull $%d, %%ecx\n\taddl %%ecx, %%eax\n", elem_size); }
                    } else {
                        if (elem_size == 1) anvil_strbuf_append(&be->code, "\tlea eax, [eax+ecx*1]\n");
                        else if (elem_size == 2) anvil_strbuf_append(&be->code, "\tlea eax, [eax+ecx*2]\n");
                        else if (elem_size == 4) anvil_strbuf_append(&be->code, "\tlea eax, [eax+ecx*4]\n");
                        else if (elem_size == 8) anvil_strbuf_append(&be->code, "\tlea eax, [eax+ecx*8]\n");
                        else { anvil_strbuf_appendf(&be->code, "\timul ecx, %d\n\tadd eax, ecx\n", elem_size); }
                    }
                }
            }
            break;
            
        case ANVIL_OP_STRUCT_GEP:
            {
                x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
                int offset = 0;
                if (instr->aux_type && instr->aux_type->kind == ANVIL_TYPE_STRUCT &&
                    instr->num_operands > 1 && instr->operands[1]->kind == ANVIL_VAL_CONST_INT) {
                    unsigned idx = (unsigned)instr->operands[1]->data.i;
                    if (idx < instr->aux_type->data.struc.num_fields)
                        offset = (int)instr->aux_type->data.struc.offsets[idx];
                }
                if (offset != 0) {
                    if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_appendf(&be->code, "\taddl $%d, %%eax\n", offset);
                    else anvil_strbuf_appendf(&be->code, "\tadd eax, %d\n", offset);
                }
            }
            break;
            
        case ANVIL_OP_BR:
            if (instr->true_block) {
                anvil_strbuf_appendf(&be->code, "\tjmp .L%s_%s\n", be->current_func->name, instr->true_block->name);
            }
            break;
            
        case ANVIL_OP_BR_COND:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_append(&be->code, "\ttestl %eax, %eax\n");
            else anvil_strbuf_append(&be->code, "\ttest eax, eax\n");
            if (instr->true_block && instr->false_block) {
                anvil_strbuf_appendf(&be->code, "\tjnz .L%s_%s\n", be->current_func->name, instr->true_block->name);
                anvil_strbuf_appendf(&be->code, "\tjmp .L%s_%s\n", be->current_func->name, instr->false_block->name);
            }
            break;
            
        case ANVIL_OP_RET:
            if (instr->num_operands > 0 && instr->operands[0])
                x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_epilogue(be, syntax);
            break;
            
        case ANVIL_OP_CALL:
            for (int i = (int)instr->num_operands - 1; i >= 1; i--) {
                x86_emit_load_value(be, instr->operands[i], X86_EAX, syntax);
                if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_append(&be->code, "\tpushl %eax\n");
                else anvil_strbuf_append(&be->code, "\tpush eax\n");
            }
            anvil_strbuf_appendf(&be->code, "\tcall %s\n", instr->operands[0]->name);
            if (instr->num_operands > 1) {
                if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_appendf(&be->code, "\taddl $%zu, %%esp\n", (instr->num_operands - 1) * 4);
                else anvil_strbuf_appendf(&be->code, "\tadd esp, %zu\n", (instr->num_operands - 1) * 4);
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
                x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
                x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_append(&be->code, "\tcmpl %ecx, %eax\n");
                    anvil_strbuf_appendf(&be->code, "\t%s %%al\n", setcc);
                    anvil_strbuf_append(&be->code, "\tmovzbl %al, %eax\n");
                } else {
                    anvil_strbuf_append(&be->code, "\tcmp eax, ecx\n");
                    anvil_strbuf_appendf(&be->code, "\t%s al\n", setcc);
                    anvil_strbuf_append(&be->code, "\tmovzx eax, al\n");
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
                x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
                x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
                if (syntax == ANVIL_SYNTAX_GAS) {
                    anvil_strbuf_append(&be->code, "\tcmpl %ecx, %eax\n");
                    anvil_strbuf_appendf(&be->code, "\t%s %%al\n", setcc);
                    anvil_strbuf_append(&be->code, "\tmovzbl %al, %eax\n");
                } else {
                    anvil_strbuf_append(&be->code, "\tcmp eax, ecx\n");
                    anvil_strbuf_appendf(&be->code, "\t%s al\n", setcc);
                    anvil_strbuf_append(&be->code, "\tmovzx eax, al\n");
                }
            }
            break;
            
        case ANVIL_OP_TRUNC:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            break;
            
        case ANVIL_OP_ZEXT:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            if (instr->operands[0]->type) {
                if (instr->operands[0]->type->kind == ANVIL_TYPE_I8 || instr->operands[0]->type->kind == ANVIL_TYPE_U8) {
                    if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_append(&be->code, "\tmovzbl %al, %eax\n");
                    else anvil_strbuf_append(&be->code, "\tmovzx eax, al\n");
                } else if (instr->operands[0]->type->kind == ANVIL_TYPE_I16 || instr->operands[0]->type->kind == ANVIL_TYPE_U16) {
                    if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_append(&be->code, "\tmovzwl %ax, %eax\n");
                    else anvil_strbuf_append(&be->code, "\tmovzx eax, ax\n");
                }
            }
            break;
            
        case ANVIL_OP_SEXT:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            if (instr->operands[0]->type) {
                if (instr->operands[0]->type->kind == ANVIL_TYPE_I8) {
                    if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_append(&be->code, "\tmovsbl %al, %eax\n");
                    else anvil_strbuf_append(&be->code, "\tmovsx eax, al\n");
                } else if (instr->operands[0]->type->kind == ANVIL_TYPE_I16) {
                    if (syntax == ANVIL_SYNTAX_GAS) anvil_strbuf_append(&be->code, "\tmovswl %ax, %eax\n");
                    else anvil_strbuf_append(&be->code, "\tmovsx eax, ax\n");
                }
            }
            break;
            
        case ANVIL_OP_BITCAST: case ANVIL_OP_PTRTOINT: case ANVIL_OP_INTTOPTR:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            break;
            
        case ANVIL_OP_SELECT:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            x86_emit_load_value(be, instr->operands[2], X86_EDX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\ttestl %eax, %eax\n\tcmovzl %edx, %ecx\n\tmovl %ecx, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\ttest eax, eax\n\tcmovz ecx, edx\n\tmov eax, ecx\n");
            }
            break;
            
        /* Floating-point operations using SSE2 */
        case ANVIL_OP_FADD:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovd %eax, %xmm0\n\tmovd %ecx, %xmm1\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\taddsd %xmm1, %xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\taddss %xmm1, %xmm0\n");
                anvil_strbuf_append(&be->code, "\tmovd %xmm0, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovd xmm0, eax\n\tmovd xmm1, ecx\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\taddsd xmm0, xmm1\n");
                else
                    anvil_strbuf_append(&be->code, "\taddss xmm0, xmm1\n");
                anvil_strbuf_append(&be->code, "\tmovd eax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FSUB:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovd %eax, %xmm0\n\tmovd %ecx, %xmm1\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tsubsd %xmm1, %xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\tsubss %xmm1, %xmm0\n");
                anvil_strbuf_append(&be->code, "\tmovd %xmm0, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovd xmm0, eax\n\tmovd xmm1, ecx\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tsubsd xmm0, xmm1\n");
                else
                    anvil_strbuf_append(&be->code, "\tsubss xmm0, xmm1\n");
                anvil_strbuf_append(&be->code, "\tmovd eax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FMUL:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovd %eax, %xmm0\n\tmovd %ecx, %xmm1\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tmulsd %xmm1, %xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\tmulss %xmm1, %xmm0\n");
                anvil_strbuf_append(&be->code, "\tmovd %xmm0, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovd xmm0, eax\n\tmovd xmm1, ecx\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tmulsd xmm0, xmm1\n");
                else
                    anvil_strbuf_append(&be->code, "\tmulss xmm0, xmm1\n");
                anvil_strbuf_append(&be->code, "\tmovd eax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FDIV:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovd %eax, %xmm0\n\tmovd %ecx, %xmm1\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tdivsd %xmm1, %xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\tdivss %xmm1, %xmm0\n");
                anvil_strbuf_append(&be->code, "\tmovd %xmm0, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovd xmm0, eax\n\tmovd xmm1, ecx\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tdivsd xmm0, xmm1\n");
                else
                    anvil_strbuf_append(&be->code, "\tdivss xmm0, xmm1\n");
                anvil_strbuf_append(&be->code, "\tmovd eax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FNEG:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\txorl $0x80000000, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\txor eax, 0x80000000\n");
            }
            break;
            
        case ANVIL_OP_FABS:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tandl $0x7FFFFFFF, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tand eax, 0x7FFFFFFF\n");
            }
            break;
            
        case ANVIL_OP_FCMP:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            x86_emit_load_value(be, instr->operands[1], X86_ECX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovd %eax, %xmm0\n\tmovd %ecx, %xmm1\n");
                anvil_strbuf_append(&be->code, "\tucomiss %xmm1, %xmm0\n");
                anvil_strbuf_append(&be->code, "\tseta %al\n\tmovzbl %al, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovd xmm0, eax\n\tmovd xmm1, ecx\n");
                anvil_strbuf_append(&be->code, "\tucomiss xmm0, xmm1\n");
                anvil_strbuf_append(&be->code, "\tseta al\n\tmovzx eax, al\n");
            }
            break;
            
        case ANVIL_OP_SITOFP:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvtsi2sd %eax, %xmm0\n\tmovq %xmm0, %eax\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvtsi2ss %eax, %xmm0\n\tmovd %xmm0, %eax\n");
            } else {
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvtsi2sd xmm0, eax\n\tmovq eax, xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvtsi2ss xmm0, eax\n\tmovd eax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_UITOFP:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovl %eax, %eax\n");  /* Zero-extend */
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvtsi2sd %eax, %xmm0\n\tmovq %xmm0, %eax\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvtsi2ss %eax, %xmm0\n\tmovd %xmm0, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmov eax, eax\n");
                if (instr->result && instr->result->type && instr->result->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvtsi2sd xmm0, eax\n\tmovq eax, xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvtsi2ss xmm0, eax\n\tmovd eax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FPTOSI:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovd %eax, %xmm0\n");
                if (instr->operands[0]->type && instr->operands[0]->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvttsd2si %xmm0, %eax\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvttss2si %xmm0, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovd xmm0, eax\n");
                if (instr->operands[0]->type && instr->operands[0]->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvttsd2si eax, xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvttss2si eax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FPTOUI:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovd %eax, %xmm0\n");
                if (instr->operands[0]->type && instr->operands[0]->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvttsd2si %xmm0, %eax\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvttss2si %xmm0, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovd xmm0, eax\n");
                if (instr->operands[0]->type && instr->operands[0]->type->kind == ANVIL_TYPE_F64)
                    anvil_strbuf_append(&be->code, "\tcvttsd2si eax, xmm0\n");
                else
                    anvil_strbuf_append(&be->code, "\tcvttss2si eax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FPEXT:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovd %eax, %xmm0\n\tcvtss2sd %xmm0, %xmm0\n\tmovq %xmm0, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovd xmm0, eax\n\tcvtss2sd xmm0, xmm0\n\tmovq eax, xmm0\n");
            }
            break;
            
        case ANVIL_OP_FPTRUNC:
            x86_emit_load_value(be, instr->operands[0], X86_EAX, syntax);
            if (syntax == ANVIL_SYNTAX_GAS) {
                anvil_strbuf_append(&be->code, "\tmovq %eax, %xmm0\n\tcvtsd2ss %xmm0, %xmm0\n\tmovd %xmm0, %eax\n");
            } else {
                anvil_strbuf_append(&be->code, "\tmovq xmm0, eax\n\tcvtsd2ss xmm0, xmm0\n\tmovd eax, xmm0\n");
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
    
    /* Emit label with function prefix */
    if (block != be->current_func->blocks) {
        anvil_strbuf_appendf(&be->code, ".L%s_%s:\n", be->current_func->name, block->name);
    }
    
    /* Emit instructions */
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        x86_emit_instr(be, instr, syntax);
    }
}

static void x86_emit_func(x86_backend_t *be, anvil_func_t *func, anvil_syntax_t syntax)
{
    if (!func || func->is_declaration) return;
    
    be->current_func = func;
    be->num_stack_slots = 0;
    be->next_stack_offset = 0;
    
    /* First pass: count stack slots needed */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_ALLOCA) {
                x86_add_stack_slot(be, instr->result);
            }
        }
    }
    
    /* Calculate stack size (16-byte aligned) */
    func->stack_size = (be->next_stack_offset + 15) & ~15;
    if (func->stack_size < 16) func->stack_size = 16;
    
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
    .reset = x86_reset,
    .codegen_module = x86_codegen_module,
    .codegen_func = x86_codegen_func,
    .get_arch_info = x86_get_arch_info
};
