/*
 * ANVIL - ARM64 Backend Internal Definitions
 * 
 * Internal structures and helpers for the ARM64 code generator.
 */

#ifndef ARM64_INTERNAL_H
#define ARM64_INTERNAL_H

#include "anvil/anvil_internal.h"
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
 * Value Location Tracking
 * ============================================================================ */

typedef enum {
    ARM64_LOC_NONE,      /* Not yet assigned */
    ARM64_LOC_REG,       /* In a register */
    ARM64_LOC_STACK,     /* On the stack */
    ARM64_LOC_CONST,     /* Constant value (immediate) */
    ARM64_LOC_GLOBAL,    /* Global variable/function */
} arm64_loc_kind_t;

typedef struct {
    arm64_loc_kind_t kind;
    union {
        int reg;              /* Register number (for LOC_REG) */
        int stack_offset;     /* Stack offset from FP (for LOC_STACK) */
        int64_t imm;          /* Immediate value (for LOC_CONST) */
        const char *name;     /* Symbol name (for LOC_GLOBAL) */
    };
    int size;                 /* Size in bytes (1, 2, 4, 8) */
    int reg_class;            /* GPR or FPR */
    bool is_signed;           /* For integer types */
} arm64_value_loc_t;

/* ============================================================================
 * Stack Frame Layout
 * ============================================================================ */

/*
 * ARM64 Stack Frame Layout (stack grows down):
 *
 * Higher addresses
 * +---------------------------+
 * | Caller's frame            |
 * +---------------------------+
 * | Return address (x30)      | <- Old SP
 * | Saved FP (x29)            |
 * +---------------------------+ <- FP (x29)
 * | Callee-saved registers    |
 * | (x19-x28 as needed)       |
 * +---------------------------+
 * | Local variables           |
 * | (alloca results)          |
 * +---------------------------+
 * | Spill slots               |
 * | (for register spills)     |
 * +---------------------------+
 * | Outgoing arguments        |
 * | (for calls with >8 args)  |
 * +---------------------------+ <- SP (16-byte aligned)
 * Lower addresses
 */

typedef struct {
    int callee_saved_offset;  /* Offset to callee-saved area from FP */
    int callee_saved_size;    /* Size of callee-saved area */
    int locals_offset;        /* Offset to locals area from FP */
    int locals_size;          /* Size of locals area */
    int spill_offset;         /* Offset to spill area from FP */
    int spill_size;           /* Size of spill area */
    int outgoing_offset;      /* Offset to outgoing args from FP */
    int outgoing_size;        /* Size of outgoing args area */
    int total_size;           /* Total frame size (16-byte aligned) */
} arm64_frame_layout_t;

/* ============================================================================
 * Stack Slot Management
 * ============================================================================ */

typedef struct {
    anvil_value_t *value;     /* Associated IR value */
    int offset;               /* Offset from FP (negative) */
    int size;                 /* Size in bytes */
    bool is_param;            /* Is this a parameter slot? */
    bool is_alloca;           /* Is this an alloca slot? */
} arm64_stack_slot_t;

/* ============================================================================
 * Register State
 * ============================================================================ */

typedef struct {
    anvil_value_t *value;     /* Current value in register, or NULL */
    bool is_dirty;            /* Value modified, needs writeback */
    bool is_locked;           /* Cannot be spilled (e.g., during instruction) */
} arm64_reg_state_t;

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
    
    /* Label generation */
    int label_counter;
    int string_counter;
    
    /* Current function being generated */
    anvil_func_t *current_func;
    
    /* Context reference for ABI detection */
    anvil_ctx_t *ctx;
    
    /* Stack frame layout */
    arm64_frame_layout_t frame;
    
    /* Stack slots */
    arm64_stack_slot_t *stack_slots;
    size_t num_stack_slots;
    size_t stack_slots_cap;
    int next_stack_offset;
    
    /* Register state */
    arm64_reg_state_t gpr[ARM64_NUM_GPR];
    arm64_reg_state_t fpr[ARM64_NUM_FPR];
    uint32_t used_callee_saved;  /* Bitmask of used callee-saved regs */
    
    /* Value locations (indexed by value ID) */
    arm64_value_loc_t *value_locs;
    size_t value_locs_cap;
    
    /* String table */
    arm64_string_entry_t *strings;
    size_t num_strings;
    size_t strings_cap;
    
    /* Statistics for debugging */
    int total_instrs;
    int total_spills;
    int total_reloads;
} arm64_backend_t;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/* ABI detection */
static inline bool arm64_is_darwin(arm64_backend_t *be) {
    return be->ctx && be->ctx->abi == ANVIL_ABI_DARWIN;
}

static inline const char *arm64_symbol_prefix(arm64_backend_t *be) {
    return arm64_is_darwin(be) ? "_" : "";
}

/* Register names */
extern const char *arm64_xreg_names[33];  /* x0-x30, sp, xzr */
extern const char *arm64_wreg_names[33];  /* w0-w30, wsp, wzr */
extern const char *arm64_dreg_names[32];  /* d0-d31 */
extern const char *arm64_sreg_names[32];  /* s0-s31 */

/* Get register name based on size */
const char *arm64_reg_name(int reg, int size, int reg_class);

/* Type size calculation */
int arm64_type_size(anvil_type_t *type);
int arm64_type_align(anvil_type_t *type);
bool arm64_type_is_float(anvil_type_t *type);
bool arm64_type_is_signed(anvil_type_t *type);

/* Stack slot management */
int arm64_alloc_stack_slot(arm64_backend_t *be, anvil_value_t *val, int size);
int arm64_get_stack_slot(arm64_backend_t *be, anvil_value_t *val);
int arm64_get_or_alloc_slot(arm64_backend_t *be, anvil_value_t *val);

/* Value location management */
arm64_value_loc_t *arm64_get_value_loc(arm64_backend_t *be, anvil_value_t *val);
void arm64_set_value_loc(arm64_backend_t *be, anvil_value_t *val, arm64_value_loc_t *loc);

/* Register allocation helpers */
int arm64_alloc_temp_reg(arm64_backend_t *be);
void arm64_free_temp_reg(arm64_backend_t *be, int reg);
int arm64_alloc_callee_saved(arm64_backend_t *be);

/* Code emission helpers */
void arm64_emit_mov_imm(arm64_backend_t *be, int reg, int64_t imm);
void arm64_emit_load_from_stack(arm64_backend_t *be, int reg, int offset, int size);
void arm64_emit_load_from_stack_signed(arm64_backend_t *be, int reg, int offset, int size, bool is_signed);
void arm64_emit_store_to_stack(arm64_backend_t *be, int reg, int offset, int size);
void arm64_emit_load_global(arm64_backend_t *be, int reg, const char *name);

/* Frame management */
void arm64_analyze_function(arm64_backend_t *be, anvil_func_t *func);
void arm64_emit_prologue(arm64_backend_t *be, anvil_func_t *func);
void arm64_emit_epilogue(arm64_backend_t *be);

/* String table */
const char *arm64_add_string(arm64_backend_t *be, const char *str);

/* ============================================================================
 * Instruction Emission (arm64_emit.c)
 * ============================================================================ */

/* Value loading */
void arm64_emit_load_value(arm64_backend_t *be, anvil_value_t *val, int target_reg);
void arm64_emit_load_fp_value(arm64_backend_t *be, anvil_value_t *val, int target_dreg);
void arm64_save_result(arm64_backend_t *be, anvil_instr_t *instr);

/* PHI handling */
void arm64_emit_phi_copies(arm64_backend_t *be, anvil_block_t *src_block, anvil_block_t *dest_block);

/* Instruction emission */
void arm64_emit_instr(arm64_backend_t *be, anvil_instr_t *instr);

/* Memory operations */
void arm64_emit_load(arm64_backend_t *be, anvil_instr_t *instr);
void arm64_emit_store(arm64_backend_t *be, anvil_instr_t *instr);
void arm64_emit_gep(arm64_backend_t *be, anvil_instr_t *instr);
void arm64_emit_struct_gep(arm64_backend_t *be, anvil_instr_t *instr);

/* Comparison */
void arm64_emit_cmp(arm64_backend_t *be, anvil_instr_t *instr);

/* Control flow */
void arm64_emit_br(arm64_backend_t *be, anvil_instr_t *instr);
void arm64_emit_br_cond(arm64_backend_t *be, anvil_instr_t *instr);
void arm64_emit_call(arm64_backend_t *be, anvil_instr_t *instr);
void arm64_emit_ret(arm64_backend_t *be, anvil_instr_t *instr);

/* Type conversions */
void arm64_emit_convert(arm64_backend_t *be, anvil_instr_t *instr);

/* Floating-point */
void arm64_emit_fp(arm64_backend_t *be, anvil_instr_t *instr);

#endif /* ARM64_INTERNAL_H */
