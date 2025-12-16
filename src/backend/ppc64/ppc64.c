/*
 * ANVIL - PowerPC 64-bit Backend (Big-Endian)
 * 
 * Big-endian, stack grows downward
 * Generates GAS syntax for PowerPC64
 * 
 * Register conventions (ELFv1 ABI for PPC64 BE):
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
 * Stack frame (ELFv1):
 * - Minimum frame size: 112 bytes
 * - Parameter save area starts at SP+48
 * - TOC save area at SP+40
 * - LR save area at SP+16
 */

#include "ppc64_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Global Data
 * ============================================================================ */

/* PowerPC 64-bit register names */
const char *ppc64_gpr_names[] = {
    "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
    "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23",
    "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31"
};

const char *ppc64_fpr_names[] = {
    "f0",  "f1",  "f2",  "f3",  "f4",  "f5",  "f6",  "f7",
    "f8",  "f9",  "f10", "f11", "f12", "f13", "f14", "f15",
    "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23",
    "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31"
};

/* Argument registers */
const int ppc64_arg_regs[] = { PPC64_R3, PPC64_R4, PPC64_R5, PPC64_R6, PPC64_R7, PPC64_R8, PPC64_R9, PPC64_R10 };

static const anvil_arch_info_t ppc64_arch_info = {
    .arch = ANVIL_ARCH_PPC64,
    .name = "PowerPC 64-bit",
    .ptr_size = 8,
    .addr_bits = 64,
    .word_size = 8,
    .num_gpr = 32,
    .num_fpr = 32,
    .endian = ANVIL_ENDIAN_BIG,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false
};

/* ============================================================================
 * Backend Initialization
 * ============================================================================ */

static anvil_error_t ppc64_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    ppc64_backend_t *priv = calloc(1, sizeof(ppc64_backend_t));
    if (!priv) return ANVIL_ERR_NOMEM;
    
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->label_counter = 0;
    priv->ctx = ctx;
    
    be->priv = priv;
    return ANVIL_OK;
}

static void ppc64_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    ppc64_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv->strings);
    free(priv->stack_slots);
    free(priv);
    be->priv = NULL;
}

/* ============================================================================
 * Stack Slot Management
 * ============================================================================ */

int ppc64_add_stack_slot(ppc64_backend_t *be, anvil_value_t *val)
{
    if (be->num_stack_slots >= be->stack_slots_cap) {
        size_t new_cap = be->stack_slots_cap ? be->stack_slots_cap * 2 : 16;
        ppc64_stack_slot_t *new_slots = realloc(be->stack_slots,
            new_cap * sizeof(ppc64_stack_slot_t));
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

int ppc64_get_stack_slot(ppc64_backend_t *be, anvil_value_t *val)
{
    for (size_t i = 0; i < be->num_stack_slots; i++) {
        if (be->stack_slots[i].value == val) {
            return be->stack_slots[i].offset;
        }
    }
    return -1;
}

const char *ppc64_add_string(ppc64_backend_t *be, const char *str)
{
    for (size_t i = 0; i < be->num_strings; i++) {
        if (strcmp(be->strings[i].str, str) == 0) {
            return be->strings[i].label;
        }
    }
    
    if (be->num_strings >= be->strings_cap) {
        size_t new_cap = be->strings_cap ? be->strings_cap * 2 : 16;
        ppc64_string_entry_t *new_strings = realloc(be->strings,
            new_cap * sizeof(ppc64_string_entry_t));
        if (!new_strings) return ".str_err";
        be->strings = new_strings;
        be->strings_cap = new_cap;
    }
    
    ppc64_string_entry_t *entry = &be->strings[be->num_strings];
    entry->str = str;
    entry->len = strlen(str);
    snprintf(entry->label, sizeof(entry->label), ".LC%d", be->string_counter++);
    be->num_strings++;
    
    return entry->label;
}

/* ============================================================================
 * Backend Interface
 * ============================================================================ */

static const anvil_arch_info_t *ppc64_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &ppc64_arch_info;
}

static anvil_error_t ppc64_codegen_module(anvil_backend_t *be, anvil_module_t *mod,
                                           char **output, size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;
    
    ppc64_backend_t *priv = be->priv;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->label_counter = 0;
    priv->num_strings = 0;
    priv->string_counter = 0;
    
    /* Emit header with CPU model info */
    anvil_strbuf_append(&priv->code, "# Generated by ANVIL for PowerPC 64-bit (big-endian, ELFv1 ABI)\n");
    
    /* Emit CPU-specific directive */
    ppc64_emit_cpu_directive(priv);
    
    anvil_strbuf_append(&priv->code, "\t.abiversion 1\n");
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
            ppc64_emit_func(priv, func);
        }
    }
    
    /* Emit globals */
    ppc64_emit_globals(priv, mod);
    
    /* Emit strings */
    ppc64_emit_strings(priv);
    
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

static anvil_error_t ppc64_codegen_func(anvil_backend_t *be, anvil_func_t *func,
                                         char **output, size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    
    ppc64_backend_t *priv = be->priv;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);
    
    ppc64_emit_func(priv, func);
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

const anvil_backend_ops_t anvil_backend_ppc64 = {
    .name = "PowerPC 64-bit",
    .arch = ANVIL_ARCH_PPC64,
    .init = ppc64_init,
    .cleanup = ppc64_cleanup,
    .codegen_module = ppc64_codegen_module,
    .codegen_func = ppc64_codegen_func,
    .get_arch_info = ppc64_get_arch_info
};
