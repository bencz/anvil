/* AT&T assembly the i386 (gas32.c) and x86-64 (gas64.c) GAS printers spell
 * alike: condition codes, floating compares on the ucomis/fucomip flags,
 * block labels and conditional branches. */
#ifndef ANVIL_X86_GAS_H
#define ANVIL_X86_GAS_H

#include "anvil/anvil_internal.h"
#include "anvil/anvil_machine.h"
#include "../../common/mir_isel.h"
#include "../../common/print_flags.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* What both printers keep while they print a function, the first member of
 * each one's own state: the allocated function, the text, the frame
 * layout, the blocks' layout and the compare, carry and load folds in
 * progress. */
typedef struct {
    const anvil_mir_func_t *mir;
    anvil_syntax_t syntax;
    anvil_strbuf_t code;
    int *spill_offsets;
    size_t num_spill_offsets;
    int *frame_slot_offsets;
    size_t num_frame_slot_offsets;
    int outgoing_size;
    int frame_size;
    /* The prologue at the MIR's frame block (anvil_mir_isel_frameless_prefix). */
    bool wrapped;
    bool emitted_fneg_mask;
    bool emitted_fabs_mask;
    bool failed;
    anvil_mir_isel_facts_t isel;
    anvil_mir_layout_t layout; /* blocks printed, and where branches land */
    anvil_mir_block_t block;   /* being emitted (blocks print in order) */
    const char *fused_jcc;     /* set by a compare folded into the next BR_COND or SELECT */
    /* x + (a u< b): the extension at adc_skip prints nothing, the add at
     * adc_at is adc $0 on the compare's carry (SIZE_MAX: none). */
    size_t adc_skip, adc_at;
    /* x & c read only by `== 0` / `!= 0` feeding the branch: printed as
     * test, the compare at test_at prints nothing (SIZE_MAX: none). */
    size_t test_at;
    anvil_mir_vreg_t adc_other;
    anvil_print_flags_t flags; /* what the flags tell */
    /* A load folded into a later instruction as its memory operand: that
     * instruction's index and the address. */
    size_t fold_target, fold_load;
    char fold_address[400];
    /* The mode, as each printer states it: the pointer width, the stack
     * pointer, the register frame displacements count from and what is
     * added to them (set once the frame is laid out), the narrowest move of
     * a general register to or from a spill slot or a stack argument, the
     * bytes below the outgoing stack arguments (Win64's home area), which
     * registers have a byte form (NULL: all), and the FP unit's moves
     * between a register and memory. */
    int pointer_bytes;
    const char *symbol_prefix;
    /* Constants live in 32-bit registers: an immediate takes any value
     * modulo 2^32 (else a value must fit a sign-extended imm32). */
    bool wraps_imm32;
    /* SSE arithmetic takes a folded load as its memory operand. */
    bool fp_memory_operands;
    const char *stack_reg;
    const char *frame_reg;
    int frame_bias;
    int min_gpr_move;
    /* Integer operations narrower than this compute at this width (the
     * low bytes are the same); shifts keep their own width. */
    int min_operation_bytes;
    int outgoing_base;
    /* Where the incoming stack arguments start from the frame base, and
     * the register a dynamic allocation computes its size in. */
    int incoming_base;
    int alloca_scratch;
    bool (*has_byte)(int phys);
    void (*fp_load)(void *gas, anvil_mir_vreg_t vreg, const char *mem);
    void (*fp_store)(void *gas, anvil_mir_vreg_t vreg, const char *mem);
    /* The FP unit's compare of `a` with `b` into the flags (as ucomis b, a). */
    void (*fp_compare)(void *gas, anvil_mir_vreg_t a, anvil_mir_vreg_t b);
    /* The FP unit's ADD, SUB, MUL and DIV, and the printer's integer
     * division (the operands' registers given). */
    void (*fp_binary)(void *gas, anvil_mir_opcode_t op, anvil_mir_vreg_t def, anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs);
    void (*gpr_divide)(void *gas, size_t index, const anvil_mir_instr_info_t *info, int dst_phys, int a_phys, int b_phys, int size);
    /* The FP unit's copies (to, from or between FP registers) and FP
     * constants (their bits). */
    void (*fp_copy)(void *gas, anvil_mir_vreg_t dst, anvil_mir_vreg_t src);
    void (*fp_constant)(void *gas, anvil_mir_vreg_t dst, uint64_t bits);
    /* The FP unit's NEG and FABS. */
    void (*fp_unary)(void *gas, anvil_mir_opcode_t op, anvil_mir_vreg_t dst, anvil_mir_vreg_t src);
    /* The printer's own instructions (atomics, vector and scalar forms,
     * conversions, symbol addresses, VA_START, dynamic allocation, incoming
     * arguments, calls, returns and their pseudos); false for one it does
     * not print. */
    bool (*target_instr)(void *gas, size_t index, const anvil_mir_instr_info_t *info);
    /* Touches each page a stack allocation of the size in alloca_scratch
     * crosses (NULL: the system grows the stack on any access). */
    void (*stack_probe)(void *gas, size_t index);
    const char *local_label_prefix;
    /* Frame operands formatted for the instruction being printed. */
    char frame_text[8][24];
    unsigned frame_next;
} x86_gas_t;

/* A general register's name at `bytes` (1, 2, 4 or 8) and an XMM
 * register's ("?" out of range); i386 uses the first eight. */
const char *x86_gas_gpr(int phys, int bytes);
const char *x86_gas_xmm(int phys);
/* The AT&T operand-size suffix of `bytes` (1, 2, 4 or 8). */
const char *x86_gas_size_suffix(int bytes);
/* The register an allocated vreg holds (-1, marking the printing failed,
 * when it was spilled or not allocated). */
int x86_gas_phys_of(x86_gas_t *gas, anvil_mir_vreg_t vreg);

/* The prefix a reference to `symbol` takes (none for a local label). */
const char *x86_gas_symbol_ref_prefix(const x86_gas_t *gas, const char *symbol);
/* A vreg the isel facts know as a constant that fits an instruction's
 * immediate (wrapped to 32 bits when `wraps_imm32`). */
bool x86_gas_facts_imm(const anvil_mir_isel_facts_t *facts, bool wraps_imm32, anvil_mir_vreg_t vreg, int64_t *out);
bool x86_gas_known_imm(const x86_gas_t *gas, anvil_mir_vreg_t vreg, int64_t *out);
/* The isel facts' callback: whether the printers encode operand `use` of
 * the instruction as an immediate (`ctx` is the printer state). */
bool x86_gas_use_takes_immediate(void *ctx, size_t index, const anvil_mir_instr_info_t *info, size_t use, int64_t value);
/* The AT&T memory operand of the access at `index` whose base is use
 * `base_use` (an absolute symbol where the isel facts fold it). */
bool x86_gas_memory_operand(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info, size_t base_use, char *out, size_t out_len);
/* Folds the load at `index` into the instruction reading it, a few
 * instructions on (a compare, or an arithmetic operation's second
 * operand), as its memory operand; false when it does not fold. */
bool x86_gas_fold_load(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info, const anvil_mir_vreg_info_t *def_info);
/* Marks the operands the printers encode with no register (immediates
 * only: symbols stay materialized), for a mode set up by `mode`. */
bool x86_gas_mark_registerless(anvil_mir_func_t *mir, void (*mode)(x86_gas_t *gas, const void *user), const void *user);
/* ADD, SUB, MUL, AND, OR, XOR and the shifts on general registers: lea
 * and shift forms where they do the work in one instruction, an
 * immediate where the facts give one; false for any other operation (the
 * printer's division). */
bool x86_gas_emit_gpr_arith(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info, int dst_phys, int a_phys, int b_phys, int size);
/* Whether the compare at `index` only feeds the branch or select right
 * after it (which then reads the flags). */
bool x86_gas_fuses(const x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info);
/* An integer or FP compare: the flags for a fused branch or select, a
 * carry for an add of the condition (adc), or a 0/1 register. */
void x86_gas_emit_cmp(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info);
/* A binary operation: into a folded load's reader, as a test feeding the
 * next branch (x & c compared with zero), on the FP unit, or on general
 * registers. */
void x86_gas_emit_binary(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info);
/* COPY (a general register at least min_operation_bytes wide; the FP
 * unit's otherwise) and MOV of a constant (xor for zero, the shortest
 * move otherwise). */
void x86_gas_emit_copy(x86_gas_t *gas, anvil_mir_vreg_t dst, anvil_mir_vreg_t src);
void x86_gas_emit_mov(x86_gas_t *gas, const anvil_mir_instr_info_t *info);
/* NEG, NOT and FABS. */
void x86_gas_emit_unary(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info);
/* ZEXT and SEXT of a general register: nothing or a copy where the value
 * is already extended (a narrow load of its signedness, a value known zero
 * above its width, a 32-bit operation's result on x86-64), movz/movs
 * otherwise. */
void x86_gas_emit_gpr_extend(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info, bool sign_extend);
/* One MIR instruction: the shared forms here, the printer's own through
 * target_instr. */
void x86_gas_emit_instr(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info);
/* INCOMING_STACK_ARG and DYN_ALLOCA (the outgoing area moved below the
 * new block). */
void x86_gas_emit_incoming_stack_arg(x86_gas_t *gas, const anvil_mir_instr_info_t *info);
void x86_gas_emit_dyn_alloca(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info);
/* LOAD (narrow integers extended to 32 bits) and STORE (an immediate
 * where it takes one). */
void x86_gas_emit_load(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info);
void x86_gas_emit_store(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info);

/* The frame operand `disp` bytes from the frame base (negative: locals,
 * spills and saves; positive: incoming arguments), from a small ring of
 * buffers (several per instruction). */
const char *x86_gas_frame(x86_gas_t *gas, int disp);
/* Whether the address of the access at `address_index` reads `phys`. */
bool x86_gas_address_reads(x86_gas_t *gas, size_t address_index, const anvil_mir_instr_info_t *address, int phys);
/* The anvil_parallel_move_ops_t moves of pointer-wide registers (`user`
 * is the printer state). */
void x86_gas_parallel_move(void *user, int dst, int src);
void x86_gas_parallel_swap(void *user, int a, int b);
/* FRAME_ADDR, SPILL_LOAD, SPILL_STORE, SELECT (CMOVcc on the flags a fused
 * compare left, else on the condition register) and CALL_STACK_ARG. */
void x86_gas_emit_frame_addr(x86_gas_t *gas, const anvil_mir_instr_info_t *info);
void x86_gas_emit_spill_load(x86_gas_t *gas, const anvil_mir_instr_info_t *info);
void x86_gas_emit_spill_store(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info);
void x86_gas_emit_select(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info);
void x86_gas_emit_call_stack_arg(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info);

/* The setcc mnemonic of an integer compare; the jcc suffix is the text
 * after "set". */
const char *x86_gas_setcc(anvil_mir_opcode_t op);
const char *x86_gas_jcc(anvil_mir_opcode_t op);

/* The jcc suffix of the opposite condition (NULL: none). */
const char *x86_gas_inverse_jcc(const char *cc);

/* After `ucomis b, a` (or fucomip with ST(0) = a, ST(1) = b) unordered sets
 * ZF, PF and CF, a < b CF, a == b ZF.  The predicates one condition decides,
 * some on the swapped compare; OEQ and UNE need two. */
bool x86_gas_fcmp_branch(anvil_fcmp_pred_t pred, const char **cc, bool *swap);

/* dst8 = pred on those flags (not swapped), through local branches named
 * after instruction `index`. */
void x86_gas_emit_fcmp_bool(anvil_strbuf_t *code, const char *label_prefix, const char *function, size_t index,
                            anvil_fcmp_pred_t pred, const char *dst8);

/* dst = cc ? then : else on the flags already set, through `cmov<cc>`
 * with the mov suffix `suffix` (registers named without %): `else` moves in
 * first (mov leaves the flags), or a dst already holding `then` takes
 * `else` on the inverse condition.  False for a condition without one. */
bool x86_gas_emit_cmov(anvil_strbuf_t *code, const char *cc, const char *suffix, const char *dst, const char *then_reg, const char *else_reg);

/* "<prefix><function>_<block name>", as a definition or a reference. */
bool x86_gas_emit_label(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, anvil_mir_block_t block);
bool x86_gas_emit_branch_target(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, anvil_mir_block_t block);

/* Jumps to true_block on `cc` and to false_block otherwise, from code in
 * block `current`: a target that is the next printed block falls through;
 * targets land where `layout` forwards them. */
bool x86_gas_emit_cond_branch(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, const anvil_mir_layout_t *layout,
                              anvil_mir_block_t current, const char *cc, anvil_mir_block_t true_block, anvil_mir_block_t false_block);

/* An unconditional jump, omitted to the next printed block. */
bool x86_gas_emit_jump(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, const anvil_mir_layout_t *layout,
                       anvil_mir_block_t current, anvil_mir_block_t target);

/* An integer load of `load_bits` read only by the compare at `reader`
 * of the same width: the compare can read the memory itself (`cmp %r,
 * mem` with the load on the left, `cmp mem, %r` on the right).  `other`
 * is the other operand.  Printers keep the load before a compare with an
 * immediate: `cmp $imm, mem` does not fuse with the branch after it, a
 * load and `test`/`cmp $imm, %r` do. */
bool x86_gas_compare_folds(const anvil_mir_func_t *mir, size_t reader, anvil_mir_vreg_t load, unsigned load_bits, bool *load_is_lhs,
                           anvil_mir_vreg_t *other);

/* That compare, on the memory operand `address`, against the register
 * `other_reg` (named without %). */
void x86_gas_emit_memory_compare(anvil_strbuf_t *code, const char *suffix, const char *address, bool load_is_lhs, const char *other_reg);

#endif
