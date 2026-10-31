/* Internal contracts shared by the PowerPC backend components. */

#ifndef ANVIL_PPC_INTERNAL_H
#define ANVIL_PPC_INTERNAL_H

#include "anvil/anvil_ppc_mir.h"
#include "anvil/anvil_internal.h"
#include "anvil/anvil_analysis.h"
#include "../common/aggregate_mir.h"
#include "../common/ir_lower.h"
#include "../common/vector_isa.h"

#include "../common/mir_isel.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const anvil_ppc_target_desc_t *desc;
    anvil_cpu_features_t features;  /* of the CPU being targeted */
    anvil_reloc_model_t reloc;      /* effective relocation model */
    const anvil_mir_func_t *mir;
    anvil_strbuf_t code;
    int *spill_offsets;
    size_t num_spill_offsets;
    int *frame_slot_offsets;
    size_t num_frame_slot_offsets;
    int gpr_save_offsets[32];
    int fpr_save_offsets[32];
    int outgoing_size;
    int frame_size;
    int fp_const_scratch_offset;
    size_t label_counter;
    bool has_frame;
    anvil_mir_isel_facts_t isel;
    anvil_mir_block_t block;  /* being emitted (blocks print in order) */
    const char *fused_branch; /* set by a compare folded into the next BR_COND */
    /* Set by a compare folded into the next SELECT (isel): the CR0 bit
     * (LT 0, GT 1, EQ 2) and whether the condition is its negation. */
    int fused_select_bit;
    bool fused_select_negated;
    bool failed;
} ppc_mir_emit_t;

/* Machine placement of a struct, union or array passed (BYVAL) or returned
 * (SRET) by value. */
typedef enum {
    PPC_AGG_IMAGE,          /* argument: memory image over consecutive
                               parameter doublewords (GPRs, then the stack) */
    PPC_AGG_REGS,           /* homogeneous floating argument in FPRs, or a
                               result in r3/r4 or f1-f8 */
    PPC_AGG_REFERENCE,      /* argument: pointer to a caller-owned copy */
    PPC_AGG_INDIRECT        /* result: memory addressed by the hidden first
                               argument */
} ppc_agg_placement_t;

typedef struct {
    ppc_agg_placement_t placement;
    anvil_agg_part_t parts[8];      /* PPC_AGG_REGS */
    unsigned num_parts;
    bool fp;
    size_t size;
    size_t align;
} ppc_agg_plan_t;

/* Common start of every classification: rejects non-aggregates. */
static inline bool ppc_agg_plan_init(anvil_type_t *type, ppc_agg_plan_t *plan)
{
    memset(plan, 0, sizeof(*plan));
    if (!type || (type->kind != ANVIL_TYPE_STRUCT && type->kind != ANVIL_TYPE_ARRAY) || !type->size)
        return false;
    plan->size = type->size;
    plan->align = type->align ? type->align : 1;
    return true;
}

/* Argument assignment state, advanced by the ABI module one argument
 * piece at a time. */
typedef struct {
    size_t gpr;
    size_t fpr;
    int64_t stack;      /* offset of the next stack (or parameter save
                           area) word, as the ABI module defines it */
} ppc_arg_state_t;

typedef struct {
    int reg;            /* argument register, or -1 for memory */
    int64_t offset;     /* stack argument offset when reg < 0 */
} ppc_arg_loc_t;

typedef struct anvil_ppc_abi_ops {
    const char *name;
    anvil_abi_t abi;
    anvil_syntax_t syntax;
    void (*emit_function_header)(ppc_mir_emit_t *emit);
    /* `bl symbol`; `via_got`: the callee may live in another module and
     * the code is position-independent. */
    void (*emit_direct_call)(ppc_mir_emit_t *emit, const char *symbol, bool via_got);
    /* dst = &symbol + addend, from the GOT when `got`. */
    void (*emit_symbol_address)(ppc_mir_emit_t *emit, const char *dst, const char *symbol, int64_t addend, bool got);
    void (*emit_indirect_call)(ppc_mir_emit_t *emit, const char *target_reg);
    bool (*classify_aggregate)(anvil_type_t *type, bool is_return, ppc_agg_plan_t *plan);

    /* Location of the next general word, floating value or (32-bit ABIs
     * only) 64-bit integer register pair; NULL when the ABI has none. */
    ppc_arg_loc_t (*assign_word)(const anvil_ppc_target_desc_t *desc, ppc_arg_state_t *state);
    ppc_arg_loc_t (*assign_fpr)(const anvil_ppc_target_desc_t *desc, ppc_arg_state_t *state, bool single);
    ppc_arg_loc_t (*assign_pair)(const anvil_ppc_target_desc_t *desc, ppc_arg_state_t *state, int *lo_reg);

    /* Variadic calls: unnamed floating arguments travel as general words
     * (the callee reads them from the parameter save area), the caller
     * provides that many bytes of save area, and an optional hook emits the
     * ABI's per-call setup just before the branch. */
    bool unnamed_fp_in_gprs;
    unsigned variadic_save_area;
    void (*emit_variadic_call_setup)(ppc_mir_emit_t *emit, bool fp_in_regs);
    /* The callee extends integer results narrower than this (bits) to it. */
    unsigned result_extend_bits;
    /* Registers a call may change (volatile in every PowerPC ELF ABI:
     * r0, r3-r12, f0-f13, v0-v19). */
    uint64_t call_gpr_clobbers;
    uint64_t call_fpr_clobbers;
    uint64_t call_vector_clobbers;

    /* Variadic callees: the MachineIR va_start for the named arguments'
     * state, the prologue's homing of argument registers, va_start itself
     * (into GPR `dst`), and the IR expansions of va_arg and va_copy. */
    bool (*add_va_start)(anvil_mir_func_t *mir, anvil_mir_vreg_t def, const ppc_arg_state_t *named);
    void (*emit_va_register_save)(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *va_start);
    void (*emit_va_start)(ppc_mir_emit_t *emit, int dst, const anvil_mir_instr_info_t *va_start);
    anvil_value_t *(*build_va_arg)(anvil_backend_t *be, const anvil_ppc_target_desc_t *desc, anvil_value_t *storage, anvil_type_t *type,
                                   const char *name);
    anvil_value_t *(*build_va_copy)(anvil_backend_t *be, anvil_value_t *cursor, const char *name);
    bool (*build_va_copy_into)(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor);
} ppc_abi_ops_t;

/* Emitter helpers for the ABI modules: `op reg, offset(base)` in any
 * displacement range, and dst = base + offset (both may use r0). */
void ppc_emit_mem_access(ppc_mir_emit_t *emit, const char *op, const char *reg, const char *base, int64_t offset);
void ppc_emit_addi_large(ppc_mir_emit_t *emit, int dst, int base, int64_t offset);
extern const char *const ppc_gpr_names[32];
extern const char *const ppc_fpr_names[32];

/* Variadic callee support (abi/varargs.c). */
bool ppc_elf64_add_va_start(anvil_mir_func_t *mir, anvil_mir_vreg_t def, const ppc_arg_state_t *named);
void ppc_elf64_emit_va_register_save(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *va_start);
void ppc_elf64_emit_va_start(ppc_mir_emit_t *emit, int dst, const anvil_mir_instr_info_t *va_start);
anvil_value_t *ppc_elf64_build_va_arg(anvil_backend_t *be, const anvil_ppc_target_desc_t *desc, anvil_value_t *storage, anvil_type_t *type,
                                      const char *name);
anvil_value_t *ppc_cursor_va_copy(anvil_backend_t *be, anvil_value_t *cursor, const char *name);
bool ppc_cursor_va_copy_into(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor);
bool ppc_elf32_add_va_start(anvil_mir_func_t *mir, anvil_mir_vreg_t def, const ppc_arg_state_t *named);
void ppc_elf32_emit_va_register_save(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *va_start);
void ppc_elf32_emit_va_start(ppc_mir_emit_t *emit, int dst, const anvil_mir_instr_info_t *va_start);
anvil_value_t *ppc_elf32_build_va_arg(anvil_backend_t *be, const anvil_ppc_target_desc_t *desc, anvil_value_t *storage, anvil_type_t *type,
                                      const char *name);
anvil_value_t *ppc_elf32_build_va_copy(anvil_backend_t *be, anvil_value_t *cursor, const char *name);
bool ppc_elf32_build_va_copy_into(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor);

/* Backend va hooks: dispatch to the selected variant's ABI. */
bool anvil_ppc_backend_reloc_model_supported(anvil_backend_t *be, anvil_reloc_model_t model);
anvil_value_t *anvil_ppc_backend_build_va_arg(anvil_backend_t *be, anvil_value_t *storage, anvil_type_t *type, const char *name);
anvil_value_t *anvil_ppc_backend_build_va_copy(anvil_backend_t *be, anvil_value_t *cursor, const char *name);
bool anvil_ppc_backend_build_va_copy_into(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor);

/* Parameter save area assignment shared by the 64-bit ELF ABIs. */
ppc_arg_loc_t ppc_elf64_assign_word(const anvil_ppc_target_desc_t *desc, ppc_arg_state_t *state);
/* TOC-relative address, or the TOC-addressed GOT entry. */
void ppc_elf64_emit_symbol_address(ppc_mir_emit_t *emit, const char *dst, const char *symbol, int64_t addend, bool got);
/* Emits the code for one function at the given relocation model. */
bool ppc_emit_mir_reloc(const anvil_mir_func_t *mir, anvil_ppc_variant_t variant, anvil_cpu_features_t features,
                        anvil_reloc_model_t reloc, char **output, size_t *len);
ppc_arg_loc_t ppc_elf64_assign_fpr(const anvil_ppc_target_desc_t *desc, ppc_arg_state_t *state, bool single);

/* Register shape of a target for the shared aggregate helpers. */
static inline anvil_agg_target_t ppc_agg_target_of(const anvil_ppc_target_desc_t *desc)
{
    return (anvil_agg_target_t){desc->word_size, !desc->little_endian};
}

/* Whether a conversion at `index` calls a run-time helper on a CPU with
 * `features` (the printer expands it into a call). */
bool ppc_conversion_calls_helper(const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *info, anvil_cpu_features_t features);

/* Backend operations shared by the three variants; the variant comes
 * from the backend's architecture. */
anvil_error_t anvil_ppc_backend_init(anvil_backend_t *be, anvil_ctx_t *ctx);
void anvil_ppc_backend_cleanup(anvil_backend_t *be);
void anvil_ppc_backend_reset(anvil_backend_t *be);
const anvil_arch_info_t *anvil_ppc_backend_arch_info(anvil_backend_t *be);
anvil_error_t anvil_ppc_backend_codegen_module(anvil_backend_t *be, anvil_module_t *mod, char **output, size_t *len);
anvil_error_t anvil_ppc_backend_codegen_func(anvil_backend_t *be, anvil_func_t *func, char **output, size_t *len);
bool anvil_ppc_backend_atomic_is_lock_free(anvil_backend_t *be, anvil_type_t *type);
unsigned anvil_ppc_backend_vector_operation_cost(anvil_backend_t *be, anvil_op_t operation, anvil_type_t *type, anvil_vector_shape_t shape);
/* VSX/AltiVec vector instructions (ppc_vector.c). */
extern const anvil_vector_isa_t ppc_vector_isa[];
/* The variant descriptor of a PowerPC backend. */
const anvil_ppc_target_desc_t *ppc_backend_desc(const anvil_backend_t *be);
/* Entries the variant may use (64-bit mode adds doubleword-GPR forms). */
size_t ppc_vector_isa_count(const anvil_ppc_target_desc_t *desc);

#define ANVIL_PPC_BACKEND_OPS                                       \
    .init = anvil_ppc_backend_init,                                 \
    .cleanup = anvil_ppc_backend_cleanup,                           \
    .reset = anvil_ppc_backend_reset,                               \
    .codegen_module = anvil_ppc_backend_codegen_module,             \
    .codegen_func = anvil_ppc_backend_codegen_func,                 \
    .get_arch_info = anvil_ppc_backend_arch_info,                   \
    .atomic_is_lock_free = anvil_ppc_backend_atomic_is_lock_free,   \
    .vector_operation_cost = anvil_ppc_backend_vector_operation_cost, \
    .build_va_arg = anvil_ppc_backend_build_va_arg,                 \
    .reloc_model_supported = anvil_ppc_backend_reloc_model_supported, \
    .build_va_copy = anvil_ppc_backend_build_va_copy,               \
    .build_va_copy_into = anvil_ppc_backend_build_va_copy_into,     \
    .param_attr_support = (1u << ANVIL_PARAM_BYVAL) | (1u << ANVIL_PARAM_SRET), \
    .abi_support = (1u << ANVIL_ABI_SYSV)

extern const ppc_abi_ops_t ppc_elf32_abi_ops;
extern const ppc_abi_ops_t ppc_elfv1_abi_ops;
extern const ppc_abi_ops_t ppc_elfv2_abi_ops;

extern const int ppc_gpr_arg_regs[8];
extern const int ppc32_fpr_arg_regs[8];
extern const int ppc64_fpr_arg_regs[13];
extern const int ppc_alloc_gpr_regs[25];
extern const int ppc_alloc_fpr_regs[30];
extern const int ppc_scratch_gpr_regs[2];
extern const int ppc_scratch_fpr_regs[2];
/* AltiVec registers (v n = VSX register 32 + n): allocatable, spill
 * scratch, and the printer's temporaries v0/v1, all volatile. */
enum { PPC_NUM_ALLOC_VECTOR_REGS = 16, PPC_NUM_SCRATCH_VECTOR_REGS = 2, PPC_VECTOR_TEMP = 0, PPC_VECTOR_TEMP2 = 1 };
extern const int ppc_alloc_vector_regs[PPC_NUM_ALLOC_VECTOR_REGS];
extern const int ppc_scratch_vector_regs[PPC_NUM_SCRATCH_VECTOR_REGS];
extern const anvil_ppc_target_desc_t ppc32_target_desc;
extern const anvil_ppc_target_desc_t ppc64_target_desc;
extern const anvil_ppc_target_desc_t ppc64le_target_desc;

#endif
