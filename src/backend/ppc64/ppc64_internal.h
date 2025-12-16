/*
 * ANVIL - PowerPC 64-bit Backend Internal Header
 * 
 * Shared definitions for PPC64 backend modules
 */

#ifndef PPC64_INTERNAL_H
#define PPC64_INTERNAL_H

#include "anvil/anvil_internal.h"

/* PowerPC 64-bit register names */
extern const char *ppc64_gpr_names[];
extern const char *ppc64_fpr_names[];

/* Register indices */
#define PPC64_R0   0
#define PPC64_R1   1   /* Stack pointer */
#define PPC64_R2   2   /* TOC pointer */
#define PPC64_R3   3   /* First arg / return value */
#define PPC64_R4   4
#define PPC64_R5   5
#define PPC64_R6   6
#define PPC64_R7   7
#define PPC64_R8   8
#define PPC64_R9   9
#define PPC64_R10  10
#define PPC64_R11  11
#define PPC64_R12  12  /* Function entry point */
#define PPC64_R13  13  /* Thread pointer (reserved) */
#define PPC64_R31  31  /* Frame pointer */

/* Argument registers */
extern const int ppc64_arg_regs[];
#define PPC64_NUM_ARG_REGS 8

/* ELFv1 ABI constants */
#define PPC64_MIN_FRAME_SIZE 112
#define PPC64_LR_SAVE_OFFSET 16
#define PPC64_TOC_SAVE_OFFSET 40
#define PPC64_PARAM_SAVE_OFFSET 48

/* String table entry */
typedef struct {
    const char *str;
    char label[32];
    size_t len;
} ppc64_string_entry_t;

/* Stack slot tracking */
typedef struct {
    anvil_value_t *value;
    int offset;
} ppc64_stack_slot_t;

/* Backend private data */
typedef struct {
    anvil_strbuf_t code;
    anvil_strbuf_t data;
    int label_counter;
    int string_counter;
    int stack_offset;
    int local_offset;
    size_t frame_size;
    
    /* Stack slots for local variables */
    ppc64_stack_slot_t *stack_slots;
    size_t num_stack_slots;
    size_t stack_slots_cap;
    int next_stack_offset;
    
    /* String table */
    ppc64_string_entry_t *strings;
    size_t num_strings;
    size_t strings_cap;
    
    /* Current function being generated */
    anvil_func_t *current_func;
    
    /* Context reference */
    anvil_ctx_t *ctx;
} ppc64_backend_t;

/* ============================================================================
 * Stack slot management (ppc64.c)
 * ============================================================================ */

int ppc64_add_stack_slot(ppc64_backend_t *be, anvil_value_t *val);
int ppc64_get_stack_slot(ppc64_backend_t *be, anvil_value_t *val);
const char *ppc64_add_string(ppc64_backend_t *be, const char *str);

/* ============================================================================
 * Instruction emission (ppc64_emit.c)
 * ============================================================================ */

void ppc64_emit_prologue(ppc64_backend_t *be, anvil_func_t *func);
void ppc64_emit_epilogue(ppc64_backend_t *be, anvil_func_t *func);
void ppc64_emit_load_value(ppc64_backend_t *be, anvil_value_t *val, int reg, anvil_func_t *func);
void ppc64_emit_instr(ppc64_backend_t *be, anvil_instr_t *instr, anvil_func_t *func);
void ppc64_emit_block(ppc64_backend_t *be, anvil_block_t *block, anvil_func_t *func);
void ppc64_emit_func(ppc64_backend_t *be, anvil_func_t *func);
void ppc64_emit_globals(ppc64_backend_t *be, anvil_module_t *mod);
void ppc64_emit_strings(ppc64_backend_t *be);

/* ============================================================================
 * CPU-specific code generation (ppc64_cpu.c)
 * ============================================================================ */

/* Check if a CPU feature is available */
bool ppc64_has_feature(ppc64_backend_t *be, anvil_cpu_features_t feature);

/* Get current CPU model */
anvil_cpu_model_t ppc64_get_cpu_model(ppc64_backend_t *be);

/* CPU-specific instruction emission */

/* Population count - uses popcntd on POWER5+, emulation otherwise */
void ppc64_emit_popcnt(ppc64_backend_t *be, int dest_reg, int src_reg);

/* Byte reversal - uses ldbrx/stdbrx on POWER7+, manual swap otherwise */
void ppc64_emit_bswap64(ppc64_backend_t *be, int dest_reg, int src_reg);
void ppc64_emit_bswap32(ppc64_backend_t *be, int dest_reg, int src_reg);

/* Conditional select - uses isel on POWER7+, branch otherwise */
void ppc64_emit_isel(ppc64_backend_t *be, int dest_reg, int true_reg, int false_reg, int cr_bit);

/* Compare bytes - uses cmpb on POWER6+, emulation otherwise */
void ppc64_emit_cmpb(ppc64_backend_t *be, int dest_reg, int src1_reg, int src2_reg);

/* FP copy sign - uses fcpsgn on POWER7+, manual otherwise */
void ppc64_emit_fcpsgn(ppc64_backend_t *be, int dest_fpr, int sign_fpr, int mag_fpr);

/* Vector operations (AltiVec/VSX) */
bool ppc64_can_use_altivec(ppc64_backend_t *be);
bool ppc64_can_use_vsx(ppc64_backend_t *be);

/* POWER10 specific */
bool ppc64_can_use_pcrel(ppc64_backend_t *be);
bool ppc64_can_use_mma(ppc64_backend_t *be);

/* Emit CPU model directive in assembly output */
void ppc64_emit_cpu_directive(ppc64_backend_t *be);

#endif /* PPC64_INTERNAL_H */
