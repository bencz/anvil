/*
 * ANVIL - x86 (32-bit) Backend Internal Definitions
 *
 * Register numbering, name tables, and ABI/calling-convention descriptors for
 * the x86 MachineIR backend.
 */

#ifndef X86_32_INTERNAL_H
#define X86_32_INTERNAL_H

#include "anvil/anvil_internal.h"
#include "anvil/anvil_x86_mir.h"
#include <stdbool.h>
#include <stdint.h>

#define X86_EAX 0
#define X86_ECX 1
#define X86_EDX 2
#define X86_EBX 3
#define X86_ESP 4
#define X86_EBP 5
#define X86_ESI 6
#define X86_EDI 7

#define X86_NUM_GPR 8
#define X86_NUM_FPR 8

typedef enum { X86_DECOR_NONE = 0, X86_DECOR_STDCALL, X86_DECOR_FASTCALL } x86_decor_style_t;

typedef enum { X86_PLAT_ELF = 0, X86_PLAT_MACHO, X86_PLAT_COFF } x86_platform_t;

typedef struct {
    anvil_cc_t cc;
    const char *name;
    bool callee_cleans_stack;
    int num_reg_int_args;
    const int *reg_int_args;
    x86_decor_style_t decor;
    int int_ret_reg;
    int int_ret_hi_reg;
    const int *alloc_gpr_regs;
    int num_alloc_gpr_regs;
    const int *alloc_fpr_regs;
    int num_alloc_fpr_regs;
    const int *scratch_gpr_regs;
    int num_scratch_gpr_regs;
    const int *scratch_fpr_regs;
    int num_scratch_fpr_regs;
} anvil_x86_cc_desc_t;

typedef struct {
    x86_platform_t platform;
    const char *sym_prefix;
    bool is_macho;
    bool is_coff;
} anvil_x86_plat_desc_t;

const anvil_x86_cc_desc_t *anvil_x86_get_cc_desc(anvil_cc_t cc);
const anvil_x86_plat_desc_t *anvil_x86_get_plat_desc(anvil_abi_t abi);

extern const char *x86_reg32_names[8];
extern const char *x86_reg16_names[8];
extern const char *x86_reg8_names[8];
extern const char *x86_xmm_names[8];

bool x86_reg_has_byte(int phys_reg);
const char *x86_byte_reg_name(int phys_reg);

int x86_type_size(anvil_type_t *type);
int x86_type_align(anvil_type_t *type);
bool x86_type_is_float(anvil_type_t *type);

typedef struct {
    anvil_strbuf_t code;
    anvil_strbuf_t data;
    anvil_ctx_t *ctx;
    anvil_syntax_t syntax;
} x86_backend_priv_t;

anvil_error_t x86_codegen_module(anvil_backend_t *be, anvil_module_t *mod, char **output, size_t *len);

anvil_error_t x86_codegen_func(anvil_backend_t *be, anvil_func_t *func, char **output, size_t *len);

bool x86_needs_pair(anvil_type_t *type);

int64_t x86_stack_arg_slot_size(anvil_type_t *type);

#endif
