/*
 * ANVIL - x86-64 Backend Internal Definitions
 *
 * Register numbering, name tables, and ABI descriptors for the x86-64
 * MachineIR backend.
 */

#ifndef X86_64_INTERNAL_H
#define X86_64_INTERNAL_H

#include "anvil/anvil_internal.h"
#include "anvil/anvil_x86_64_mir.h"
#include "../common/aggregate_mir.h"
#include <stdbool.h>
#include <stdint.h>

#define X64_RAX 0
#define X64_RCX 1
#define X64_RDX 2
#define X64_RBX 3
#define X64_RSP 4
#define X64_RBP 5
#define X64_RSI 6
#define X64_RDI 7
#define X64_R8 8
#define X64_R9 9
#define X64_R10 10
#define X64_R11 11
#define X64_R12 12
#define X64_R13 13
#define X64_R14 14
#define X64_R15 15

#define X64_NUM_GPR 16
#define X64_NUM_FPR 16

typedef struct {
    anvil_abi_t abi;
    const char *name;
    const char *sym_prefix;
    bool positional_args;
    bool is_darwin;
    bool is_win64;
    int shadow_space;
    const int *int_arg_regs;
    int num_int_arg_regs;
    const int *fp_arg_regs;
    int num_fp_arg_regs;
    int int_ret_reg;
    int int_ret_hi_reg;
    int fp_ret_reg;
    int indirect_call_reg;
    uint64_t call_gpr_clobbers;
    uint64_t call_fpr_clobbers;
    const int *alloc_gpr_regs;
    int num_alloc_gpr_regs;
    const int *alloc_fpr_regs;
    int num_alloc_fpr_regs;
    const int *scratch_gpr_regs;
    int num_scratch_gpr_regs;
    const int *scratch_fpr_regs;
    int num_scratch_fpr_regs;
} anvil_x64_abi_desc_t;

const anvil_x64_abi_desc_t *anvil_x64_get_abi_desc(anvil_abi_t abi);

/* Machine placement of a struct or array passed (BYVAL) or returned (SRET)
 * by value under the descriptor's ABI. */
typedef enum {
    X64_AGG_REGS,           /* parts in argument / return registers */
    X64_AGG_STACK,          /* copied into the stack argument area */
    X64_AGG_REFERENCE,      /* pointer to a caller-owned copy */
    X64_AGG_HIDDEN_POINTER  /* result written through the caller's pointer,
                               which is passed first and returned in RAX */
} x64_agg_placement_t;

/* One eightbyte: fp selects the SSE class (XMM) over INTEGER (GPR). */
typedef anvil_agg_part_t x64_agg_part_t;

typedef struct {
    x64_agg_placement_t placement;
    x64_agg_part_t parts[2];
    unsigned num_parts;
    unsigned num_gpr;
    unsigned num_fpr;
    size_t size;
    size_t align;
} x64_agg_plan_t;

/* SysV/Darwin (psABI 3.2.3) or Win64 classification of an aggregate.  For
 * arguments X64_AGG_REGS assumes enough free registers; callers fall back to
 * X64_AGG_STACK when they are not available. */
bool x64_classify_aggregate(const anvil_x64_abi_desc_t *desc, anvil_type_t *type,
                            bool is_return, x64_agg_plan_t *plan);

/* Argument assignment state, advanced one argument at a time. */
typedef struct {
    size_t gpr;
    size_t fpr;
    size_t position;        /* positional ABIs: argument slots used */
    int64_t stack;          /* offset of the next stack argument */
} x64_arg_state_t;

typedef struct {
    int reg;                /* argument register, or -1 for memory */
    int shadow_reg;         /* positional ABIs: the integer register of a
                               floating argument's slot (variadic copies) */
    int64_t offset;         /* stack argument offset when reg < 0 */
} x64_arg_loc_t;

/* Location of the next scalar argument. */
x64_arg_loc_t x64_assign_scalar(const anvil_x64_abi_desc_t *desc, x64_arg_state_t *state, bool fp);

/* Location of a BYVAL aggregate classified X64_AGG_REGS or X64_AGG_STACK:
 * regs[part] for every part when they all fit (reg >= 0), else the stack
 * offset of its memory image. */
x64_arg_loc_t x64_assign_aggregate(const anvil_x64_abi_desc_t *desc, x64_arg_state_t *state,
                                   const x64_agg_plan_t *plan, int regs[2]);

typedef struct {
    const char *str;
    char label[32];
    size_t len;
} x64_string_entry_t;

typedef struct {
    anvil_strbuf_t code;
    anvil_strbuf_t data;
    int string_counter;
    anvil_ctx_t *ctx;
    anvil_syntax_t syntax;
    x64_string_entry_t *strings;
    size_t num_strings;
    size_t strings_cap;
} x64_backend_t;

extern const char *x64_reg64_names[16];
extern const char *x64_reg32_names[16];
extern const char *x64_reg16_names[16];
extern const char *x64_reg8_names[16];
extern const char *x64_xmm_names[16];

int x64_type_size(anvil_type_t *type);
int x64_type_align(anvil_type_t *type);
bool x64_type_is_float(anvil_type_t *type);
anvil_value_t *anvil_x64_build_va_arg(anvil_backend_t *be, anvil_value_t *cursor_storage, anvil_type_t *type, const char *name);
anvil_value_t *anvil_x64_build_va_copy(anvil_backend_t *be, anvil_value_t *cursor, const char *name);
bool anvil_x64_build_va_copy_into(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor);

anvil_error_t x64_codegen_module(anvil_backend_t *be, anvil_module_t *mod, char **output, size_t *len);

anvil_error_t x64_codegen_func(anvil_backend_t *be, anvil_func_t *func, char **output, size_t *len);

anvil_error_t x64_classify_abi_value(anvil_backend_t *be, anvil_type_t *type, bool is_return, anvil_abi_value_plan_t *plan);

#endif
