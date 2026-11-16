/* The MachineIR legality check every back end shares: what a target can
 * hold and execute, stated as facts (register widths and counts, pointer
 * width, stack-argument alignment, select classes, ISA tables), with one
 * hook for what only the target knows (its ABI registers in calls,
 * VA_START, its pseudos, its address forms). */
#ifndef ANVIL_BACKEND_MIR_LEGAL_H
#define ANVIL_BACKEND_MIR_LEGAL_H

#include "anvil/anvil_internal.h"
#include "anvil/anvil_machine.h"
#include "vector_isa.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A width bit of anvil_mir_legal_target_t.widths: 8 << k bits. */
#define ANVIL_MIR_WIDTH(bits) ((uint8_t)((bits) == 8 ? 1 : (bits) == 16 ? 2 : (bits) == 32 ? 4 : (bits) == 64 ? 8 : (bits) == 128 ? 16 : 0))

typedef struct anvil_mir_legal_target anvil_mir_legal_target_t;

/* The target's own rules for `instr` (index `index`).  Returns false when it
 * is illegal (with `error` set); sets *decided when they settle it, else the
 * shared rules check it next. */
typedef bool (*anvil_mir_legal_hook_fn)(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *instr,
                                        char *error, size_t error_len, bool *decided);

struct anvil_mir_legal_target {
    const char *name;       /* in messages */
    /* Widths (ANVIL_MIR_WIDTH bits) each register class holds, and the
     * physical registers a vreg of the class may be fixed to. */
    uint8_t widths[ANVIL_MIR_REG_CLASS_COUNT];
    int num_regs[ANVIL_MIR_REG_CLASS_COUNT];
    uint16_t pointer_bits;
    /* A vector held in the FP class at this width (0: none there): only
     * vector operations, accesses, copies and spill code touch one. */
    uint16_t fp_vector_bits;
    /* Byte alignment of incoming and outgoing stack-argument offsets (0:
     * the value's own size, at most 8; 1: any, as an entry index). */
    unsigned stack_arg_align;
    /* Register classes (bit per class) the MIR SELECT picks in. */
    unsigned select_classes;
    /* The CPU's forms of MIR SCALAR and VECTOR operations. */
    const anvil_vector_isa_t *scalar_isa;
    size_t scalar_isa_count;
    const anvil_vector_isa_t *vector_isa;
    size_t vector_isa_count;
    anvil_mir_legal_hook_fn hook;
    const void *user;
};

/* The vreg's info when the target holds it (class, width, fixed register);
 * NULL with `error` set otherwise. */
const anvil_mir_vreg_info_t *anvil_mir_legal_vreg(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, anvil_mir_vreg_t vreg, size_t index,
                                                  char *error, size_t error_len);
/* `vreg` is a pointer-wide general register. */
bool anvil_mir_legal_pointer(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, anvil_mir_vreg_t vreg, size_t index, char *error,
                             size_t error_len);

/* The ABI's rules for a call: its result in a fixed register (`result_ok`
 * names the ones allowed; NULL: any fixed one when `fixed_result`), an
 * indirect target in a pointer register (fixed to `target_reg` when it is
 * not negative), and arguments in fixed registers `arg_ok` accepts (NULL:
 * any) when `fixed_args`. */
typedef struct {
    bool fixed_result;
    bool (*result_ok)(const anvil_mir_legal_target_t *target, const anvil_mir_vreg_info_t *info);
    bool fixed_target;
    int target_reg;
    bool fixed_args;
    bool (*arg_ok)(const anvil_mir_legal_target_t *target, const anvil_mir_vreg_info_t *info);
} anvil_mir_legal_call_t;

bool anvil_mir_legal_call(const anvil_mir_legal_target_t *target, const anvil_mir_legal_call_t *rules, const anvil_mir_func_t *mir, size_t index,
                          const anvil_mir_instr_info_t *instr, char *error, size_t error_len);

/* The whole function: the target-independent verifier, then every
 * instruction by the target's facts and hook. */
bool anvil_mir_verify_legal(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, char *error, size_t error_len);

/* What register allocation takes from a target (`user` is passed to every
 * hook): its legality check, the registers calls and its own instructions
 * change (NULL: none), its address forms and expensive constants for the
 * MIR optimizations, the printer's temporaries (NULL: none declared), the
 * operands that need no register, and the allocatable and scratch pools. */
typedef struct {
    bool (*verify)(const anvil_mir_func_t *mir, const void *user);
    bool (*annotate_clobbers)(anvil_mir_func_t *mir, const void *user);
    anvil_mir_address_mode_fn address_mode;
    anvil_mir_expensive_constant_fn expensive_constant;
    void *mir_user;
    bool (*annotate_printer)(anvil_mir_func_t *mir, const void *user);
    bool (*mark_registerless)(anvil_mir_func_t *mir, const void *user);
    const anvil_regalloc_class_config_t *configs;
    size_t num_configs;
    const anvil_regalloc_class_config_t *scratch_configs;
    size_t num_scratch_configs;
    const void *user;
} anvil_mir_allocation_t;

/* The pipeline every target allocates by: verified, the clobbers annotated,
 * optimized (anvil_mir_optimize_before_allocation), blocks ordered, copies
 * coalesced, early returns split, the printer's temporaries annotated,
 * verified, registerless operands marked, materialized constants hoisted,
 * allocated and verified again. */
bool anvil_mir_allocate(anvil_mir_func_t *mir, const anvil_mir_allocation_t *target);

#endif
