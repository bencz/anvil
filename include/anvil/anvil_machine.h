/*
 * ANVIL - Machine IR and register allocation infrastructure.
 *
 * This layer is target-independent backend infrastructure. Backends can lower
 * ANVIL IR into MachineIR, run allocation, then emit target assembly from the
 * allocated machine operations.
 */

#ifndef ANVIL_MACHINE_H
#define ANVIL_MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anvil.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t anvil_mir_vreg_t;
typedef uint32_t anvil_mir_block_t;

#define ANVIL_MIR_NO_VREG UINT32_MAX
#define ANVIL_MIR_NO_BLOCK UINT32_MAX

typedef enum {
    ANVIL_MIR_REG_GPR = 0,
    ANVIL_MIR_REG_FPR,
    ANVIL_MIR_REG_FLAGS,
    ANVIL_MIR_REG_SPECIAL,
    /* Vector registers in a file of their own (PowerPC AltiVec v0-v31);
     * targets whose vectors share the FP registers keep them in FPR. */
    ANVIL_MIR_REG_VECTOR,
    ANVIL_MIR_REG_CLASS_COUNT
} anvil_mir_reg_class_t;

typedef enum {
    ANVIL_MIR_OP_INVALID = 0,
    ANVIL_MIR_OP_MOV,
    ANVIL_MIR_OP_COPY,
    ANVIL_MIR_OP_ADD,
    ANVIL_MIR_OP_SUB,
    ANVIL_MIR_OP_MUL,
    ANVIL_MIR_OP_DIV,
    ANVIL_MIR_OP_SDIV,
    ANVIL_MIR_OP_UDIV,
    ANVIL_MIR_OP_FDIV,
    ANVIL_MIR_OP_SMOD,
    ANVIL_MIR_OP_UMOD,
    ANVIL_MIR_OP_AND,
    ANVIL_MIR_OP_OR,
    ANVIL_MIR_OP_XOR,
    ANVIL_MIR_OP_SHL,
    ANVIL_MIR_OP_SHR,
    ANVIL_MIR_OP_SAR,
    ANVIL_MIR_OP_NEG,
    ANVIL_MIR_OP_NOT,
    ANVIL_MIR_OP_FABS,
    ANVIL_MIR_OP_ZEXT,
    ANVIL_MIR_OP_SEXT,
    ANVIL_MIR_OP_TRUNC,
    ANVIL_MIR_OP_BITCAST,
    ANVIL_MIR_OP_SITOFP,
    ANVIL_MIR_OP_UITOFP,
    ANVIL_MIR_OP_FPTOSI,
    ANVIL_MIR_OP_FPTOUI,
    ANVIL_MIR_OP_FPEXT,
    ANVIL_MIR_OP_FPTRUNC,
    ANVIL_MIR_OP_SELECT,
    ANVIL_MIR_OP_CMP,
    ANVIL_MIR_OP_CMP_EQ,
    ANVIL_MIR_OP_CMP_NE,
    ANVIL_MIR_OP_CMP_LT,
    ANVIL_MIR_OP_CMP_LE,
    ANVIL_MIR_OP_CMP_GT,
    ANVIL_MIR_OP_CMP_GE,
    ANVIL_MIR_OP_CMP_ULT,
    ANVIL_MIR_OP_CMP_ULE,
    ANVIL_MIR_OP_CMP_UGT,
    ANVIL_MIR_OP_CMP_UGE,
    /* Floating comparison. Required immediate is anvil_fcmp_pred_t. */
    ANVIL_MIR_OP_FCMP,
    ANVIL_MIR_OP_SYMBOL_ADDR,
    ANVIL_MIR_OP_LOAD,
    ANVIL_MIR_OP_STORE,
    ANVIL_MIR_OP_FRAME_ADDR,
    ANVIL_MIR_OP_DYN_ALLOCA,
    ANVIL_MIR_OP_INCOMING_STACK_ARG,
    ANVIL_MIR_OP_CALL,
    ANVIL_MIR_OP_CALL_STACK_ARG,
    ANVIL_MIR_OP_RET,
    ANVIL_MIR_OP_BR,
    ANVIL_MIR_OP_BR_COND,
    ANVIL_MIR_OP_SPILL_LOAD,
    ANVIL_MIR_OP_SPILL_STORE,
    /* Semantic no-op that keeps all of its operands live. */
    ANVIL_MIR_OP_KEEPALIVE,
    /* Defines an additional fixed-register result of the preceding call. */
    ANVIL_MIR_OP_CALL_RESULT,
    /* Supplies a value to an additional ABI return register before RET.  The
       source need not be fixed: the target emitter performs the ABI move. */
    ANVIL_MIR_OP_RET_VALUE_PART,
    /* Target varargs cursor; immediate is the number of named ABI arguments. */
    ANVIL_MIR_OP_VA_START,
    ANVIL_MIR_OP_ATOMIC,
    /* A lane-wise vector operation: ir_op is the IR operation (ADD, FMUL,
     * SHL, SPLAT...), imm the lane width in bits.  Vector uses and the def
     * are 128-bit vregs; a scalar use is the broadcast value of SPLAT or a
     * shift count the same in every lane; a reduction (REDUCE_OR) defines a
     * lane-wide scalar. */
    ANVIL_MIR_OP_VECTOR,
    /* An IR operation on single values that the target's operation table
     * covers in its scalar shape (FMA, FSQRT): ir_op is the IR operation,
     * imm the width in bits; the def and every use are vregs of that width
     * and one class. */
    ANVIL_MIR_OP_SCALAR
} anvil_mir_opcode_t;

typedef struct anvil_mir_func anvil_mir_func_t;

typedef struct {
    anvil_mir_vreg_t dst;
    anvil_mir_vreg_t src;
} anvil_mir_parallel_copy_t;

/* Emit simultaneous virtual-register assignments before register allocation.
 * Destinations must be unique and copies must preserve class/width. Cycles
 * use fresh typed temporaries. Failure leaves instructions/vregs unchanged. */
bool anvil_mir_emit_parallel_copies(anvil_mir_func_t *func, const anvil_mir_parallel_copy_t *copies, size_t count);

typedef struct {
    anvil_mir_reg_class_t reg_class;
    uint16_t size_bits;
    bool is_signed;
    bool is_live_in;
    bool has_fixed_reg;
    int fixed_phys_reg;
} anvil_mir_vreg_info_t;

typedef struct {
    anvil_mir_reg_class_t reg_class;
    bool spilled;
    int phys_reg;
    int spill_slot;
} anvil_regalloc_assignment_t;

typedef struct {
    anvil_mir_reg_class_t reg_class;
    int num_phys_regs;
    const int *phys_regs;
    /* Registers a call preserves only in their low `preserved_bits`: a wider
     * value in one of them does not survive a call. */
    uint64_t partially_preserved;
    uint16_t preserved_bits;
    /* Arithmetic overwrites its first operand (two-address forms): a result
     * prefers the register of a first operand that dies there, which saves
     * the copy into the destination. */
    bool two_address;
    /* Values of at most `narrow_bits` bits live only in `narrow_regs`, the
     * registers with a form of that width; 0: no restriction. */
    uint16_t narrow_bits;
    uint64_t narrow_regs;
    /* Spill scratch: the printer writes these registers only where an
     * instruction declares it (its result, clobbers or printer scratch), so
     * a reloaded value stays usable for later instructions of its block. */
    bool printer_declares_writes;
} anvil_regalloc_class_config_t;

typedef struct {
    anvil_mir_reg_class_t reg_class;
    uint16_t size_bits;
} anvil_mir_spill_slot_info_t;

typedef struct {
    size_t size_bytes;
    uint32_t align_bytes;
} anvil_mir_frame_slot_info_t;

typedef struct {
    const char *label;
    const char *value;
} anvil_mir_string_literal_info_t;

typedef struct {
    const char *name;
    /* Index of the first instruction owned by this block and total ownership
       count. Instructions are not required to be contiguous; consumers must
       filter anvil_mir_instr_info_t::block rather than treating this as a
       [first_instr, first_instr + num_instrs) interval. */
    size_t first_instr;
    size_t num_instrs;
} anvil_mir_block_info_t;

typedef struct {
    anvil_mir_opcode_t op;
    anvil_mir_vreg_t def;
    size_t num_uses;
    anvil_mir_block_t block;
    anvil_mir_block_t true_block;
    anvil_mir_block_t false_block;
    bool has_imm;
    int64_t imm;
    /* Effective, target-canonical calling convention for CALL.  The
       immediate remains available for ABI-specific payloads such as the
       SysV variadic vector-register count. */
    anvil_cc_t call_cc;
    anvil_memory_access_t memory_access;
    anvil_op_t ir_op;              /* ATOMIC, VECTOR and SCALAR: the IR operation */
    anvil_atomic_info_t atomic;
    unsigned named_gpr;
    unsigned named_fpr;
    size_t named_stack_bytes;
    unsigned call_effects;
    /* Implicit physical-register writes before this instruction's results.
     * Unlisted emitter scratch registers must remain excluded from allocation. */
    uint64_t clobbers[ANVIL_MIR_REG_CLASS_COUNT];
    /* Registers the printer uses as temporaries while its operands are
     * still being read: they hold none of its operands, its result or any
     * value live across it. */
    uint64_t scratch[ANVIL_MIR_REG_CLASS_COUNT];
    const char *symbol;
    int spill_slot;
    int frame_slot;
    /* LOAD/STORE with an index register: the address is base + index *
     * index_scale + imm, the index being the use after the base. 0: none. */
    uint8_t index_scale;
    /* SYMBOL_ADDR: the address comes from the global offset table; CALL to
     * a symbol: through the PLT (see anvil_symbol_uses_got). */
    bool symbol_got;
    /* VECTOR FCMP: the predicate (the immediate is the lane width). */
    anvil_fcmp_pred_t fcmp_pred;
    /* VECTOR widening (ACCUM_*): the last use has this many times the lanes
     * of the result (2, 4, 8; 0 otherwise). */
    uint8_t widening;
} anvil_mir_instr_info_t;

/* Target addressing: may a `access_bits` load/store use base + index *
 * scale + disp? Targets without indexed modes pass no hook. */
typedef bool (*anvil_mir_address_mode_fn)(void *user, unsigned scale, int64_t disp, uint16_t access_bits);

/* Folds base + index * scale (an ADD of a MUL/SHL by a constant, or of a
 * plain index) into the loads and stores that are its only users, and
 * deletes the address arithmetic left without uses. */
bool anvil_mir_fold_address_arithmetic(anvil_mir_func_t *func, anvil_mir_address_mode_fn legal, void *user);

/* Within each block, a pure integer computation repeated with the same
 * operands becomes a copy of the first (before register allocation). */
bool anvil_mir_local_cse(anvil_mir_func_t *func);

/* Replaces an unconditional branch to a block of at most `max_instrs`
 * pure instructions ending in a conditional branch with a copy of that
 * block (rotating top-tested loops). */
bool anvil_mir_duplicate_branch_targets(anvil_mir_func_t *func, size_t max_instrs);
/* Renumbers blocks in the order of their first instructions (the lowering
 * order), which the printers then follow. */
bool anvil_mir_order_blocks(anvil_mir_func_t *func);
/* Whether instruction `index` defines a value from nothing (a constant, a
 * symbol's or a frame slot's address): spill materialization recomputes
 * such values instead of reloading them. */
bool anvil_mir_instr_recomputable(const anvil_mir_func_t *func, size_t index);

anvil_mir_func_t *anvil_mir_func_create(const char *name);
void anvil_mir_func_destroy(anvil_mir_func_t *func);

const char *anvil_mir_func_name(const anvil_mir_func_t *func);
/* Symbol binding of the function (ANVIL_LINK_EXTERNAL by default). */
void anvil_mir_func_set_linkage(anvil_mir_func_t *func, anvil_linkage_t linkage);
/* The CPU features of the processor the function is lowered for; legality
 * and printers choose instructions by them.  Lowering sets them from the
 * context; 0 (no optional feature) by default. */
void anvil_mir_func_set_cpu_features(anvil_mir_func_t *func, anvil_cpu_features_t features);
anvil_cpu_features_t anvil_mir_func_cpu_features(const anvil_mir_func_t *func);
anvil_linkage_t anvil_mir_func_linkage(const anvil_mir_func_t *func);

anvil_mir_block_t anvil_mir_add_block(anvil_mir_func_t *func,
                                      const char *name);
bool anvil_mir_set_current_block(anvil_mir_func_t *func,
                                 anvil_mir_block_t block);
anvil_mir_block_t anvil_mir_current_block(const anvil_mir_func_t *func);
size_t anvil_mir_num_blocks(const anvil_mir_func_t *func);
/* Marks a block that rarely runs (an IR cold block).  The allocator keeps
 * values out of the way of its calls (anvil_mir_split_around_cold_calls). */
bool anvil_mir_set_block_cold(anvil_mir_func_t *func, anvil_mir_block_t block, bool cold);
/* Whether the function needs a frame record (return address and frame
 * pointer saved, stack pointer moved) even with no frame bytes of its own:
 * it calls, allocates dynamically, reads stack-passed arguments or starts a
 * variadic cursor.  A function needing none and owning no frame bytes may
 * omit its prologue and epilogue. */
bool anvil_mir_needs_frame_record(const anvil_mir_func_t *func);
/* Marks in `headers` (one per block) the blocks a branch from themselves or
 * a later (not cold) block jumps to: the loop headers of the final layout,
 * which printers align (a loop's first instructions then start a fetch
 * block). */
bool anvil_mir_loop_header_blocks(const anvil_mir_func_t *func, bool *headers);
bool anvil_mir_get_block_info(const anvil_mir_func_t *func,
                              anvil_mir_block_t block,
                              anvil_mir_block_info_t *out_info);

anvil_mir_vreg_t anvil_mir_add_vreg(anvil_mir_func_t *func);
anvil_mir_vreg_t anvil_mir_add_vreg_ex(anvil_mir_func_t *func,
                                       anvil_mir_reg_class_t reg_class,
                                       uint16_t size_bits);
anvil_mir_vreg_t anvil_mir_add_vreg_typed(anvil_mir_func_t *func,
                                          anvil_mir_reg_class_t reg_class,
                                          uint16_t size_bits,
                                          bool is_signed);
const anvil_mir_vreg_info_t *
anvil_mir_get_vreg_info(const anvil_mir_func_t *func, anvil_mir_vreg_t vreg);
bool anvil_mir_set_vreg_signed(anvil_mir_func_t *func,
                               anvil_mir_vreg_t vreg,
                               bool is_signed);
bool anvil_mir_set_live_in(anvil_mir_func_t *func, anvil_mir_vreg_t vreg,
                           bool is_live_in);
bool anvil_mir_set_fixed_reg(anvil_mir_func_t *func, anvil_mir_vreg_t vreg,
                             int phys_reg);
bool anvil_mir_clear_fixed_reg(anvil_mir_func_t *func, anvil_mir_vreg_t vreg);
bool anvil_mir_add_instr(anvil_mir_func_t *func, anvil_mir_opcode_t op,
                         anvil_mir_vreg_t def,
                         const anvil_mir_vreg_t *uses,
                         size_t num_uses);
bool anvil_mir_add_atomic(anvil_mir_func_t *func, anvil_op_t operation, anvil_mir_vreg_t def,
                          const anvil_mir_vreg_t *uses, size_t count, const anvil_atomic_info_t *info);
/* ANVIL_MIR_OP_VECTOR performing `operation` on lanes of `lane_bits`. */
bool anvil_mir_add_vector(anvil_mir_func_t *func, anvil_op_t operation, anvil_mir_vreg_t def, const anvil_mir_vreg_t *uses,
                          size_t count, unsigned lane_bits);
/* ANVIL_MIR_OP_SCALAR performing `operation` on `bits`-wide values. */
bool anvil_mir_add_scalar(anvil_mir_func_t *func, anvil_op_t operation, anvil_mir_vreg_t def, const anvil_mir_vreg_t *uses,
                          size_t count, unsigned bits);
bool anvil_mir_add_instr_imm(anvil_mir_func_t *func, anvil_mir_opcode_t op,
                             anvil_mir_vreg_t def, int64_t imm);
bool anvil_mir_add_instr_imm_uses(anvil_mir_func_t *func,
                                  anvil_mir_opcode_t op,
                                  anvil_mir_vreg_t def,
                                  const anvil_mir_vreg_t *uses,
                                  size_t num_uses,
                                  int64_t imm);
bool anvil_mir_add_instr_symbol(anvil_mir_func_t *func,
                                anvil_mir_opcode_t op,
                                anvil_mir_vreg_t def,
                                const anvil_mir_vreg_t *uses,
                                size_t num_uses,
                                const char *symbol);
bool anvil_mir_add_instr_symbol_imm(anvil_mir_func_t *func,
                                    anvil_mir_opcode_t op,
                                    anvil_mir_vreg_t def,
                                    const anvil_mir_vreg_t *uses,
                                    size_t num_uses,
                                    const char *symbol,
                                    int64_t imm);
bool anvil_mir_add_call(anvil_mir_func_t *func,
                        anvil_mir_vreg_t def,
                        const anvil_mir_vreg_t *uses,
                        size_t num_uses,
                        const char *symbol,
                        anvil_cc_t call_cc,
                        bool has_abi_imm,
                        int64_t abi_imm);
/* A frame object of `size_bytes` (any size) aligned to `align_bytes` (a
 * power of two). */
int anvil_mir_add_frame_slot(anvil_mir_func_t *func,
                             size_t size_bytes,
                             uint32_t align_bytes);
/* Lays the frame slots out from byte offset `start` upward, each aligned to
 * its alignment (`default_align` when 0).  offsets[i] receives the slot's
 * start, or its end when `record_end` (frames addressed downward from a
 * frame pointer).  Fails, rather than misplacing data, when a slot needs
 * more than `max_align` (what the target's stack alignment guarantees) or
 * the frame exceeds INT_MAX. */
bool anvil_mir_layout_frame_slots(const anvil_mir_func_t *func,
                                  int start,
                                  uint32_t default_align,
                                  uint32_t max_align,
                                  bool record_end,
                                  int *offsets,
                                  int *end);
bool anvil_mir_add_frame_addr(anvil_mir_func_t *func,
                              anvil_mir_vreg_t def,
                              int frame_slot);
int anvil_mir_add_string_literal(anvil_mir_func_t *func,
                                 const char *value,
                                 const char **out_label);
bool anvil_mir_add_branch(anvil_mir_func_t *func,
                          anvil_mir_block_t target);
bool anvil_mir_add_cond_branch(anvil_mir_func_t *func,
                               anvil_mir_vreg_t cond,
                               anvil_mir_block_t true_block,
                               anvil_mir_block_t false_block);

size_t anvil_mir_num_vregs(const anvil_mir_func_t *func);
size_t anvil_mir_num_instrs(const anvil_mir_func_t *func);
bool anvil_mir_get_instr_info(const anvil_mir_func_t *func, size_t index,
                              anvil_mir_instr_info_t *out_info);

/* Attach source memory semantics to memory operations emitted since first.
 * Split accesses conservatively cap the guaranteed alignment to their width. */
bool anvil_mir_annotate_memory(anvil_mir_func_t *func, size_t first, const anvil_memory_access_t *access);
/* Calls start with ANVIL_EFFECT_ALL. A lowering may propagate an IR contract
 * to calls appended since first; helper calls from other IR operations retain
 * the conservative default. Non-call instructions carry zero call_effects. */
bool anvil_mir_annotate_call_effects(anvil_mir_func_t *func, size_t first, unsigned effects);
bool anvil_mir_set_instr_clobbers(anvil_mir_func_t *func, size_t index, anvil_mir_reg_class_t reg_class, uint64_t mask);
bool anvil_mir_set_instr_scratch(anvil_mir_func_t *func, size_t index, anvil_mir_reg_class_t reg_class, uint64_t mask);
bool anvil_mir_set_instr_symbol_got(anvil_mir_func_t *func, size_t index, bool got);
bool anvil_mir_set_instr_fcmp_pred(anvil_mir_func_t *func, size_t index, anvil_fcmp_pred_t predicate);
bool anvil_mir_set_instr_widening(anvil_mir_func_t *func, size_t index, unsigned ratio);
anvil_mir_vreg_t anvil_mir_get_instr_use(const anvil_mir_func_t *func,
                                         size_t instr_index,
                                         size_t use_index);

void anvil_mir_clear_allocations(anvil_mir_func_t *func);
bool anvil_mir_add_va_start(anvil_mir_func_t *func, anvil_mir_vreg_t def, int frame_slot, unsigned named_gpr, unsigned named_fpr, size_t named_stack_bytes);
bool anvil_mir_coalesce_copies(anvil_mir_func_t *func);
bool anvil_regalloc_linear_scan(anvil_mir_func_t *func, int num_phys_regs);
bool anvil_regalloc_linear_scan_classes(
    anvil_mir_func_t *func,
    const anvil_regalloc_class_config_t *configs,
    size_t num_configs);
bool anvil_mir_materialize_spills(
    anvil_mir_func_t *func,
    const anvil_regalloc_class_config_t *scratch_configs,
    size_t num_scratch_configs);
/* Allocation and spill materialization with each class's whole pool
 * (`configs`), the spill scratch registers (`scratch_configs`, part of the
 * pool) reloading spilled values where nothing occupies them.  Where that
 * cannot be shown, allocates again with the first 1, 2, ... scratch
 * registers of each class kept out of the pool, until all of them are. */
bool anvil_regalloc_allocate(
    anvil_mir_func_t *func,
    const anvil_regalloc_class_config_t *configs,
    size_t num_configs,
    const anvil_regalloc_class_config_t *scratch_configs,
    size_t num_scratch_configs);

const anvil_regalloc_assignment_t *
anvil_mir_get_assignment(const anvil_mir_func_t *func, anvil_mir_vreg_t vreg);
size_t anvil_mir_num_spills(const anvil_mir_func_t *func);
bool anvil_mir_get_spill_slot_info(const anvil_mir_func_t *func,
                                   int spill_slot,
                                   anvil_mir_spill_slot_info_t *out_info);
size_t anvil_mir_num_frame_slots(const anvil_mir_func_t *func);
bool anvil_mir_get_frame_slot_info(const anvil_mir_func_t *func,
                                   int frame_slot,
                                   anvil_mir_frame_slot_info_t *out_info);
size_t anvil_mir_num_string_literals(const anvil_mir_func_t *func);
bool anvil_mir_get_string_literal_info(
    const anvil_mir_func_t *func,
    size_t index,
    anvil_mir_string_literal_info_t *out_info);

bool anvil_mir_verify(const anvil_mir_func_t *func,
                      char *error,
                      size_t error_len);

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_MACHINE_H */
