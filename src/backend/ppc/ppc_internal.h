/* Internal contracts shared by the PowerPC backend components. */

#ifndef ANVIL_PPC_INTERNAL_H
#define ANVIL_PPC_INTERNAL_H

#include "anvil/anvil_ppc_mir.h"
#include "anvil/anvil_internal.h"
#include "anvil/anvil_analysis.h"
#include "../common/aggregate_mir.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const anvil_ppc_target_desc_t *desc;
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
    void (*emit_direct_call)(ppc_mir_emit_t *emit, const char *symbol);
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
} ppc_abi_ops_t;

/* Parameter save area assignment shared by the 64-bit ELF ABIs. */
ppc_arg_loc_t ppc_elf64_assign_word(const anvil_ppc_target_desc_t *desc, ppc_arg_state_t *state);
ppc_arg_loc_t ppc_elf64_assign_fpr(const anvil_ppc_target_desc_t *desc, ppc_arg_state_t *state, bool single);

/* Register shape of a target for the shared aggregate helpers. */
static inline anvil_agg_target_t ppc_agg_target_of(const anvil_ppc_target_desc_t *desc)
{
    return (anvil_agg_target_t){desc->word_size, !desc->little_endian};
}

extern const ppc_abi_ops_t ppc_elf32_abi_ops;
extern const ppc_abi_ops_t ppc_elfv1_abi_ops;
extern const ppc_abi_ops_t ppc_elfv2_abi_ops;

extern const int ppc_gpr_arg_regs[8];
extern const int ppc32_fpr_arg_regs[8];
extern const int ppc64_fpr_arg_regs[13];
extern const int ppc_alloc_gpr_regs[17];
extern const int ppc_alloc_fpr_regs[18];
extern const int ppc_scratch_gpr_regs[2];
extern const int ppc_scratch_fpr_regs[2];
extern const anvil_ppc_target_desc_t ppc32_target_desc;
extern const anvil_ppc_target_desc_t ppc64_target_desc;
extern const anvil_ppc_target_desc_t ppc64le_target_desc;

#endif
