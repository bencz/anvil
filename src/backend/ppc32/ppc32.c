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

/* Backend private data */
typedef struct {
    anvil_strbuf_t code;
    anvil_strbuf_t data;
    int label_counter;
    int stack_offset;
    int local_offset;
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
    (void)ctx;
    
    ppc32_backend_t *priv = calloc(1, sizeof(ppc32_backend_t));
    if (!priv) return ANVIL_ERR_NOMEM;
    
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->label_counter = 0;
    
    be->priv = priv;
    return ANVIL_OK;
}

static void ppc32_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    ppc32_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv);
    be->priv = NULL;
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
            
        case ANVIL_VAL_INSTR:
            /* Result of previous instruction - assume in r3 */
            if (reg != PPC_R3) {
                anvil_strbuf_appendf(&be->code, "\tmr %s, r3\n", ppc32_gpr_names[reg]);
            }
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
            
        case ANVIL_OP_LOAD:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            anvil_strbuf_append(&be->code, "\tlwz r3, 0(r3)\n");
            break;
            
        case ANVIL_OP_STORE:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            ppc32_emit_load_value(be, instr->operands[1], PPC_R4, func);
            anvil_strbuf_append(&be->code, "\tstw r3, 0(r4)\n");
            break;
            
        case ANVIL_OP_BR:
            anvil_strbuf_appendf(&be->code, "\tb .L%s\n", instr->true_block->name);
            break;
            
        case ANVIL_OP_BR_COND:
            ppc32_emit_load_value(be, instr->operands[0], PPC_R3, func);
            anvil_strbuf_append(&be->code, "\tcmpwi cr0, r3, 0\n");
            anvil_strbuf_appendf(&be->code, "\tbne cr0, .L%s\n", instr->true_block->name);
            anvil_strbuf_appendf(&be->code, "\tb .L%s\n", instr->false_block->name);
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
            
        default:
            anvil_strbuf_appendf(&be->code, "\t# unimplemented op %d\n", instr->op);
            break;
    }
}

static void ppc32_emit_block(ppc32_backend_t *be, anvil_block_t *block, anvil_func_t *func)
{
    if (!block) return;
    
    anvil_strbuf_appendf(&be->code, ".L%s:\n", block->name);
    
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        ppc32_emit_instr(be, instr, func);
    }
}

static void ppc32_emit_func(ppc32_backend_t *be, anvil_func_t *func)
{
    if (!func) return;
    
    func->stack_size = 64; /* Default stack size */
    
    ppc32_emit_prologue(be, func);
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        ppc32_emit_block(be, block, func);
    }
    
    anvil_strbuf_appendf(&be->code, "\t.size %s, .-%s\n\n", func->name, func->name);
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
    
    /* Emit header */
    anvil_strbuf_append(&priv->code, "# Generated by ANVIL for PowerPC 32-bit (big-endian)\n");
    anvil_strbuf_append(&priv->code, "\t.text\n");
    
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
    
    /* Emit data section if needed */
    if (mod->num_globals > 0 || priv->data.len > 0) {
        anvil_strbuf_append(&priv->code, "\t.data\n");
        if (priv->data.data) {
            anvil_strbuf_append(&priv->code, priv->data.data);
        }
    }
    
    *output = anvil_strbuf_detach(&priv->code, len);
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
