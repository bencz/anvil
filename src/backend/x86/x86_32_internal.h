/*
 * ANVIL - x86 (32-bit) Backend Internal Definitions
 *
 * Register numbering, name tables, and ABI/calling-convention descriptors for
 * the x86 MachineIR backend.
 */

#ifndef X86_32_INTERNAL_H
#define X86_32_INTERNAL_H

#include "../common/obj_format.h"
#include "anvil/anvil_internal.h"
#include "anvil/anvil_x86_mir.h"
#include "../common/aggregate_mir.h"
#include "../common/vector_isa.h"
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
    /* Registers a call to a function of this convention may change. */
    uint64_t call_gpr_clobbers;
    uint64_t call_fpr_clobbers;
} anvil_x86_cc_desc_t;

typedef struct {
    x86_platform_t platform;
    const char *sym_prefix;
    bool is_macho;
    bool is_coff;
    /* Aggregate results: 1/2/4/8-byte aggregates come back in EAX[:EDX]
     * and one float/double member in ST(0); otherwise through a hidden
     * pointer, which a callee_pops_sret callee drops with `ret $4`. */
    bool small_aggregates_in_regs;
    bool float_aggregates_in_x87;
    bool callee_pops_sret;
    /* Alignment ESP must have at every call (bytes). */
    int stack_alignment;
    const anvil_obj_format_t *format;
} anvil_x86_plat_desc_t;

/* Placement of a struct, union or array passed (BYVAL) or returned (SRET)
 * by value. */
typedef enum {
    X86_AGG_STACK,          /* argument: memory image in 4-byte stack slots */
    X86_AGG_REGS,           /* result in EAX[:EDX] or ST(0) */
    X86_AGG_INDIRECT        /* result through the hidden first argument */
} x86_agg_placement_t;

typedef struct {
    x86_agg_placement_t placement;
    anvil_agg_part_t parts[2];      /* X86_AGG_REGS: EAX, EDX (or one FP) */
    unsigned num_parts;
    size_t size;
    size_t align;
} x86_agg_plan_t;

bool x86_classify_aggregate(const anvil_x86_plat_desc_t *plat, anvil_type_t *type, bool is_return,
                            x86_agg_plan_t *plan);

/* Bytes a callee drops from the stack on return: all stack arguments for
 * callee-cleanup conventions, else the hidden result pointer where the
 * platform pops it. */
int64_t x86_callee_pop_bytes(const anvil_x86_cc_desc_t *desc, const anvil_x86_plat_desc_t *plat,
                             int64_t stack_arg_bytes, bool hidden_result_pointer);

extern const anvil_agg_target_t x86_agg_target;

/* Variadic arguments (abi/x86_32_varargs.c). */
anvil_value_t *anvil_x86_build_va_arg(anvil_backend_t *be, anvil_value_t *storage, anvil_type_t *type, const char *name);
anvil_value_t *anvil_x86_build_va_copy(anvil_backend_t *be, anvil_value_t *cursor, const char *name);
bool anvil_x86_build_va_copy_into(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor);

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

/* Scalar operations the IR prices on single values (FSQRT, MULH); no
 * vectors. */
extern const anvil_vector_isa_t x86_scalar_isa[];
extern const size_t x86_scalar_isa_count;
unsigned x86_scalar_operation_cost(anvil_backend_t *be, anvil_op_t operation, anvil_type_t *type, anvil_vector_shape_t shape);

int64_t x86_stack_arg_slot_size(anvil_type_t *type);

/* Records on each instruction the GPRs the GAS printer (asm/gas32.c)
 * overwrites in emitting it: as printer scratch where it writes them before
 * reading every operand, as clobbers where only values living across the
 * instruction are at risk. */
bool x86_annotate_printer_registers(anvil_mir_func_t *mir);

/* Emits one function at a relocation model (position independence on
 * ELF only). */
bool x86_emit_mir_reloc(const anvil_mir_func_t *mir, anvil_func_t *func, anvil_abi_t abi, anvil_syntax_t syntax,
                        anvil_cpu_features_t features, anvil_reloc_model_t reloc, char **output, size_t *len);

/* The COFF stdcall/fastcall spelling of `name` (false: undecorated). */
bool x86_decorated_symbol(const anvil_x86_plat_desc_t *plat, const anvil_x86_cc_desc_t *desc, anvil_type_t *fn_type,
                          const char *name, char *out, size_t out_size);

#endif
