/* Internal contracts shared by the SystemZ backend components. */

#ifndef ANVIL_SYSTEMZ_INTERNAL_H
#define ANVIL_SYSTEMZ_INTERNAL_H

#include "anvil/anvil_mainframe_mir.h"
#include "anvil/anvil_internal.h"
#include "anvil/anvil_analysis.h"
#include "../common/aggregate_mir.h"
#include "../common/mir_isel.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SYSTEMZ_INTERNAL_LABEL_CAP = 160 };

typedef struct {
    const anvil_mainframe_target_desc_t *desc;
    const anvil_mir_func_t *mir;
    anvil_fp_format_t fp_format;
    anvil_cpu_features_t features;  /* of the CPU model being targeted */
    anvil_strbuf_t code;
    char func_label[96];

    int *frame_offsets;
    int *spill_offsets;
    int frame_size;
    int outgoing_values_offset;
    int outgoing_param_list_offset;
    int *call_arg_value_offsets;
    bool *call_arg_is_last;
    size_t *call_arg_counts;
    size_t max_call_args;
    unsigned label_counter;
    /* RET_VALUE_PART's vreg (the low word of a pair result), moved by RET. */
    anvil_mir_vreg_t ret_low_word;
    anvil_mir_isel_facts_t isel;
    anvil_mir_block_t block;     /* being emitted (blocks print in order) */
    const char *fused_branch;    /* set by a compare folded into the next BR_COND */
} systemz_emit_t;

/* These linkage emitters require the HLASM printer and a caller-owned arena.
 * They do not implement storage acquisition or Language Environment linkage.
 */
typedef struct anvil_systemz_abi_ops {
    const char *name;
    anvil_abi_t abi;
    /* Aggregates.  A BYVAL argument's parameter-list slot holds the address
     * of a copy the caller makes for that call (and the callee may modify);
     * an SRET result is written through the hidden first argument, which
     * the callee also returns in the result GPR when sret_returns_address. */
    bool sret_returns_address;
    /* 64-bit integers where GPRs hold 32 bits: an argument takes two
     * consecutive parameter-list entries (high word first, the words
     * contiguous in the value area); a result returns its high word in the
     * result GPR and its low word in pair_low_gpr (-1: no word pairs). */
    int pair_low_gpr;
    void (*emit_prologue)(systemz_emit_t *emit);
    void (*emit_epilogue)(systemz_emit_t *emit);
} systemz_abi_ops_t;

typedef struct anvil_systemz_asm_ops {
    const char *name;
    anvil_syntax_t syntax;
    bool (*emit_mir)(const anvil_mir_func_t *mir, anvil_mainframe_variant_t variant, anvil_fp_format_t fp_format,
                     anvil_cpu_features_t features, char **output, size_t *len);
    bool (*emit_globals)(anvil_strbuf_t *out, anvil_module_t *mod, anvil_fp_format_t fp_format, size_t pointer_size);
    /* Once per module, before the functions: section, addressing modes,
     * register equates and symbol bindings. */
    void (*emit_module_begin)(anvil_strbuf_t *out, const anvil_module_t *mod, const anvil_mainframe_target_desc_t *desc);
    void (*emit_module_end)(anvil_strbuf_t *out);
} systemz_asm_ops_t;

extern const systemz_abi_ops_t systemz_mvs_arena_31_abi_ops;
extern const systemz_abi_ops_t systemz_mvs_arena_64_abi_ops;
extern const systemz_asm_ops_t systemz_hlasm_ops;

extern const int systemz_alloc_gprs[7];
extern const int systemz_scratch_gprs[2];
extern const int systemz_s370_fprs[1];
extern const int systemz_s370_scratch_fprs[2];
extern const int systemz_s390_fprs[13];
extern const int systemz_s390_scratch_fprs[2];
/* R12 setup for the function entry (R15 = entry point) and the label of
 * its literal pool (emitted at the function's end). */
void systemz_emit_establish_base(systemz_emit_t *emit);

/* Variadic support (the MVS parameter list as a cursor). */
anvil_value_t *anvil_systemz_build_va_arg(anvil_backend_t *be, anvil_value_t *storage, anvil_type_t *type, const char *name);
anvil_value_t *anvil_systemz_build_va_copy(anvil_backend_t *be, anvil_value_t *cursor, const char *name);
bool anvil_systemz_build_va_copy_into(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor);
void systemz_emit_literal_pool(systemz_emit_t *emit);

extern const anvil_mainframe_target_desc_t s370_target_desc;
extern const anvil_mainframe_target_desc_t s370_xa_target_desc;
extern const anvil_mainframe_target_desc_t s390_target_desc;
extern const anvil_mainframe_target_desc_t zarch_target_desc;

void systemz_uppercase(char *dest, const char *src, size_t max_len);
/* The HLASM spelling (at most 8 characters) of a source symbol, and
 * whether it differs from the plain uppercase name (an ALIAS then carries
 * the source spelling for externals). */
void systemz_symbol_name(char *dest, size_t max_len, const char *src);
bool systemz_symbol_is_renamed(const char *src);
void systemz_emit_external(anvil_strbuf_t *out, const char *directive, const char *src);
/* IBM hexadecimal floating point encoding of `value` in 4 or 8 bytes. */
bool systemz_hfp_bits(double value, size_t size, uint64_t *bits);
/* Reloads and recomputed values that spill materialization may place among
 * a call's stack arguments. */
bool systemz_is_call_preparation(const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *instr);
void systemz_emit_load_imm(systemz_emit_t *emit, int reg, int64_t imm);
bool systemz_emit_mir_ex(const anvil_mir_func_t *mir, anvil_mainframe_variant_t variant, anvil_fp_format_t fp_format,
                         anvil_cpu_features_t features, char **output, size_t *len);
bool systemz_emit_globals(anvil_strbuf_t *out, anvil_module_t *mod, anvil_fp_format_t fp_format, size_t pointer_size);

#endif
