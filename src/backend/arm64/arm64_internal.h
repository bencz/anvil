/*
 * ANVIL - ARM64 Backend Internal Definitions
 * 
 * Internal structures and helpers for the ARM64 code generator.
 */

#ifndef ARM64_INTERNAL_H
#define ARM64_INTERNAL_H

#include "anvil/anvil_internal.h"
#include "../common/obj_format.h"
#include "anvil/anvil_machine.h"
#include "../common/aggregate_mir.h"
#include "../common/ir_lower.h"
#include "../common/vector_isa.h"
#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * Register Definitions
 * ============================================================================ */

/* ARM64 general-purpose registers (64-bit) */
#define ARM64_X0   0
#define ARM64_X1   1
#define ARM64_X2   2
#define ARM64_X3   3
#define ARM64_X4   4
#define ARM64_X5   5
#define ARM64_X6   6
#define ARM64_X7   7
#define ARM64_X8   8   /* Indirect result location register */
#define ARM64_X9   9   /* Temporary */
#define ARM64_X10  10  /* Temporary */
#define ARM64_X11  11  /* Temporary */
#define ARM64_X12  12  /* Temporary */
#define ARM64_X13  13  /* Temporary */
#define ARM64_X14  14  /* Temporary */
#define ARM64_X15  15  /* Temporary */
#define ARM64_X16  16  /* IP0 - Intra-procedure scratch */
#define ARM64_X17  17  /* IP1 - Intra-procedure scratch */
#define ARM64_X18  18  /* Platform register (reserved) */
#define ARM64_X19  19  /* Callee-saved */
#define ARM64_X20  20  /* Callee-saved */
#define ARM64_X21  21  /* Callee-saved */
#define ARM64_X22  22  /* Callee-saved */
#define ARM64_X23  23  /* Callee-saved */
#define ARM64_X24  24  /* Callee-saved */
#define ARM64_X25  25  /* Callee-saved */
#define ARM64_X26  26  /* Callee-saved */
#define ARM64_X27  27  /* Callee-saved */
#define ARM64_X28  28  /* Callee-saved */
#define ARM64_FP   29  /* Frame pointer (x29) */
#define ARM64_LR   30  /* Link register (x30) */
#define ARM64_SP   31  /* Stack pointer */
#define ARM64_XZR  32  /* Zero register (special encoding) */

#define ARM64_NUM_GPR       32
#define ARM64_NUM_FPR       32
#define ARM64_NUM_ARG_REGS  8

/* Register classes */
#define ARM64_REG_CLASS_NONE     0
#define ARM64_REG_CLASS_GPR      1
#define ARM64_REG_CLASS_FPR      2

/* ============================================================================
 * Aggregates passed (BYVAL) or returned (SRET) by value - AAPCS64
 * ============================================================================ */

typedef enum {
    ARM64_AGG_REGS,         /* HFA in v registers, else 1..2 x registers;
                               arguments fall back to the stack when the
                               registers run out */
    ARM64_AGG_REFERENCE,    /* argument: pointer to a caller-owned copy */
    ARM64_AGG_INDIRECT      /* result: memory whose address is passed in x8 */
} arm64_agg_placement_t;

typedef struct {
    arm64_agg_placement_t placement;
    anvil_agg_part_t parts[4];
    unsigned num_parts;
    bool fp;                /* homogeneous floating aggregate */
    size_t size;
    size_t align;
} arm64_agg_plan_t;

/* Argument assignment state and result, advanced by the ABI one argument
 * at a time. */
typedef struct {
    size_t gpr;
    size_t fpr;
    int64_t stack;          /* offset of the next stack argument slot */
} arm64_arg_state_t;

typedef struct {
    int reg;                /* first x/v register number, or -1 for memory */
    int64_t offset;         /* stack argument offset when reg < 0 */
} arm64_arg_loc_t;

/* Procedure call standard and object format of one ARM64 ABI.  Linux
 * (abi/linux.c, AAPCS64 with ELF) and Darwin (abi/darwin.c, Apple's
 * AAPCS64 variant with Mach-O) each provide one; abi/aapcs64.c holds the
 * rules they share. */
typedef struct arm64_abi_desc {
    anvil_abi_t abi;
    const char *name;

    /* Argument passing. */
    unsigned num_gpr_args;
    unsigned num_fpr_args;
    int indirect_result_reg;        /* address of a result in memory */
    int indirect_call_reg;          /* target of an indirect call */
    bool caller_extends_small_ints; /* 8/16-bit integer arguments in
                                       registers arrive extended to 32 bits */
    bool (*classify_aggregate)(anvil_type_t *type, bool is_return, arm64_agg_plan_t *plan);
    arm64_arg_loc_t (*assign_scalar)(const struct arm64_abi_desc *desc, arm64_arg_state_t *state,
                                     bool fp, unsigned size, bool unnamed);
    arm64_arg_loc_t (*assign_aggregate)(const struct arm64_abi_desc *desc, arm64_arg_state_t *state,
                                        const arm64_agg_plan_t *plan, bool unnamed);

    /* Variadic functions: the MachineIR form of va_start given what the
     * named parameters used, its prologue register save (NULL when the
     * ABI has none) and its emission, and the IR-level va_arg / va_copy. */
    bool (*add_va_start)(anvil_mir_func_t *mir, anvil_mir_vreg_t def, const arm64_arg_state_t *named);
    void (*emit_va_register_save)(anvil_strbuf_t *code, int frame_offset);
    void (*emit_va_start)(anvil_strbuf_t *code, const char *dst, int frame_offset,
                          const anvil_mir_instr_info_t *instr);
    anvil_value_t *(*build_va_arg)(anvil_backend_t *be, anvil_value_t *storage, anvil_type_t *type, const char *name);
    anvil_value_t *(*build_va_copy)(anvil_backend_t *be, anvil_value_t *cursor, const char *name);
    bool (*build_va_copy_into)(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor);

    /* Object format and assembler spelling.  Symbols passed to the hooks
     * already carry symbol_prefix. */
    const char *symbol_prefix;
    const char *module_header;
    const char *cstring_section;
    void (*emit_function_header)(anvil_strbuf_t *code, const char *symbol, anvil_linkage_t linkage);
    void (*emit_function_footer)(anvil_strbuf_t *code, const char *symbol);
    void (*emit_symbol_address)(anvil_strbuf_t *code, const char *dst, const char *symbol, int64_t addend);
    /* A symbol's address from its GOT entry, plus `addend`. */
    void (*emit_symbol_got)(anvil_strbuf_t *code, const char *dst, const char *symbol, int64_t addend);
    /* Relocation models the ABI can produce (bit per anvil_reloc_model_t)
     * and the one DEFAULT means. */
    unsigned reloc_models;
    anvil_reloc_model_t default_reloc;
    const anvil_obj_format_t *format;
    /* The callee extends integer results narrower than this (bits) to it;
     * 0 when the ABI leaves the upper bits unspecified. */
    unsigned result_extend_bits;
} arm64_abi_desc_t;

const arm64_abi_desc_t *arm64_get_abi_desc(anvil_abi_t abi);
extern const arm64_abi_desc_t arm64_linux_abi;
extern const arm64_abi_desc_t arm64_darwin_abi;

/* Rules shared by both ABIs (abi/aapcs64.c). */
bool arm64_aapcs64_classify_aggregate(anvil_type_t *type, bool is_return, arm64_agg_plan_t *plan);
/* The next `count` consecutive x (or v) registers, else -1 after closing
 * that register class. */
int arm64_aapcs64_take_regs(size_t *next, unsigned limit, unsigned count);

/* AAPCS64 register convention (Linux and Darwin alike): the allocatable
 * pools, caller-saved first so values that do not cross a call need no
 * save, the spill scratch registers, and what a call clobbers. x9-x11 are
 * atomic-sequence temporaries, x16/x17 printer temporaries, x18 the
 * platform register, v30/v31 the printer's vector temporaries (never an
 * operand: spill reloads use the scratch registers): none is allocated. */
typedef struct {
    const int *gprs;
    int num_gprs;
    const int *fprs;
    int num_fprs;
    const int *scratch_gprs;
    int num_scratch_gprs;
    const int *scratch_fprs;
    int num_scratch_fprs;
    uint64_t call_gpr_clobbers;
    uint64_t call_fpr_clobbers;
    /* v8-v15 keep only their low 64 bits across a call. */
    uint64_t fpr_partially_preserved;
    uint16_t fpr_preserved_bits;
    int vector_temps[2];
} arm64_register_convention_t;

extern const arm64_register_convention_t arm64_aapcs64_registers;
/* Stack slot of an aggregate's memory image: doubleword slots, quadword
 * aligned for 16-byte aligned types. */
int64_t arm64_aapcs64_aggregate_stack(arm64_arg_state_t *state, const arm64_agg_plan_t *plan);

/* Emission helpers shared with the ABI modules. */
void arm64_emit_mov_imm(anvil_strbuf_t *code, const char *reg, int64_t imm);
/* reg += imm (64-bit register; x16 holds a large immediate). */
void arm64_emit_add_imm(anvil_strbuf_t *code, const char *reg, int64_t imm);
void arm64_emit_frame_access(anvil_strbuf_t *code, const char *op, const char *reg, int offset);

/* Register shape for the shared aggregate helpers. */
extern const anvil_agg_target_t arm64_agg_target;

/* ============================================================================
 * String Table Entry
 * ============================================================================ */

typedef struct {
    const char *str;
    char label[32];
    size_t len;
} arm64_string_entry_t;

/* ============================================================================
 * Backend State
 * ============================================================================ */

typedef struct {
    /* Output buffers */
    anvil_strbuf_t code;
    anvil_strbuf_t data;
    
    int string_counter;
    
    /* Context reference for ABI detection */
    anvil_ctx_t *ctx;
    
    /* String table */
    arm64_string_entry_t *strings;
    size_t num_strings;
    size_t strings_cap;
    
} arm64_backend_t;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/* The ABI descriptor of the backend's context (NULL when unsupported). */
static inline const arm64_abi_desc_t *arm64_backend_abi(const arm64_backend_t *be) {
    return be->ctx ? arm64_get_abi_desc(be->ctx->abi) : NULL;
}

/* Register names */
/* NEON vector instructions (arm64_vector.c). */
extern const anvil_vector_isa_t arm64_vector_isa[];
extern const size_t arm64_vector_isa_count;
unsigned arm64_vector_operation_cost(anvil_backend_t *be, anvil_op_t operation, anvil_type_t *type, anvil_vector_shape_t shape);

extern const char *arm64_xreg_names[33];  /* x0-x30, sp, xzr */
extern const char *arm64_wreg_names[33];  /* w0-w30, wsp, wzr */
extern const char *arm64_dreg_names[32];  /* d0-d31 */
extern const char *arm64_qreg_names[32];  /* q0-q31: 128-bit vectors */
extern const char *arm64_sreg_names[32];  /* s0-s31 */

/* Type size calculation */
int arm64_type_size(anvil_type_t *type);
int arm64_type_align(anvil_type_t *type);
bool arm64_type_is_float(anvil_type_t *type);


#endif /* ARM64_INTERNAL_H */
