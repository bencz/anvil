/* Internal contracts shared by the SystemZ backend components. */

#ifndef ANVIL_SYSTEMZ_INTERNAL_H
#define ANVIL_SYSTEMZ_INTERNAL_H

#include "anvil/anvil_mainframe_mir.h"
#include "anvil/anvil_internal.h"
#include "anvil/anvil_analysis.h"

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
} systemz_emit_t;

/* These linkage emitters require the HLASM printer and a caller-owned arena.
 * They do not implement storage acquisition or Language Environment linkage.
 */
typedef struct anvil_systemz_abi_ops {
    const char *name;
    anvil_abi_t abi;
    void (*emit_prologue)(systemz_emit_t *emit);
    void (*emit_epilogue)(systemz_emit_t *emit);
} systemz_abi_ops_t;

typedef struct anvil_systemz_asm_ops {
    const char *name;
    anvil_syntax_t syntax;
    bool (*emit_mir)(const anvil_mir_func_t *mir, anvil_mainframe_variant_t variant, anvil_fp_format_t fp_format, char **output, size_t *len);
    bool (*emit_globals)(anvil_strbuf_t *out, anvil_module_t *mod, anvil_fp_format_t fp_format, size_t pointer_size);
    void (*emit_module_end)(anvil_strbuf_t *out);
} systemz_asm_ops_t;

extern const systemz_abi_ops_t systemz_mvs_arena_31_abi_ops;
extern const systemz_abi_ops_t systemz_mvs_arena_64_abi_ops;
extern const systemz_asm_ops_t systemz_hlasm_ops;

extern const int systemz_alloc_gprs[9];
extern const int systemz_scratch_gprs[2];
extern const int systemz_s370_fprs[3];
extern const int systemz_s370_scratch_fprs[1];
extern const int systemz_s390_fprs[15];
extern const int systemz_s390_scratch_fprs[1];
extern const anvil_mainframe_target_desc_t s370_target_desc;
extern const anvil_mainframe_target_desc_t s370_xa_target_desc;
extern const anvil_mainframe_target_desc_t s390_target_desc;
extern const anvil_mainframe_target_desc_t zarch_target_desc;

void systemz_uppercase(char *dest, const char *src, size_t max_len);
bool systemz_is_call_preparation(const anvil_mir_instr_info_t *instr);
void systemz_emit_load_imm(systemz_emit_t *emit, int reg, int64_t imm);
bool systemz_emit_mir_ex(const anvil_mir_func_t *mir, anvil_mainframe_variant_t variant, anvil_fp_format_t fp_format, char **output, size_t *len);
bool systemz_emit_globals(anvil_strbuf_t *out, anvil_module_t *mod, anvil_fp_format_t fp_format, size_t pointer_size);

#endif
