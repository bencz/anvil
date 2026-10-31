/* State shared by the i386 GAS printer (gas32.c) and its floating-point
 * units: SSE2 (gas32_sse2.c) and the x87 FPU (gas32_x87.c).  The unit is
 * chosen from the CPU model's features; MachineIR is the same for both. */
#ifndef ANVIL_X86_GAS32_INTERNAL_H
#define ANVIL_X86_GAS32_INTERNAL_H

#include "../x86_32_internal.h"
#include "../../common/mir_isel.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct x86_fp_unit x86_fp_unit_t;

typedef struct {
    const anvil_mir_func_t *mir;
    const anvil_func_t *source_func;
    const anvil_x86_cc_desc_t *desc;
    const anvil_x86_plat_desc_t *plat;
    const x86_fp_unit_t *fp;
    anvil_cpu_features_t features;
    anvil_reloc_model_t reloc;      /* STATIC, or position-independent (ELF) */
    unsigned pic_labels;
    anvil_syntax_t syntax;
    anvil_strbuf_t code;
    int *spill_offsets;
    size_t num_spill_offsets;
    int *frame_slot_offsets;
    size_t num_frame_slot_offsets;
    int gpr_save_offsets[8];
    int scratch_a_off;
    int scratch_b_off;  /* 8 bytes; its high word is scratch_a */
    int fp_area_off;    /* the FP unit's frame area, at -fp_area_off(%ebp) */
    int outgoing_size;
    int frame_size;
    bool fp_returns;
    /* RET_VALUE_PART's register, moved to EDX together with RET's EAX. */
    int ret_hi_phys;
    /* The CALL already moved EDX into the following CALL_RESULT. */
    bool call_hi_moved;
    anvil_mir_isel_facts_t isel;
    anvil_mir_block_t block;     /* being emitted */
    const char *fused_jcc;       /* set by a compare folded into the next BR_COND */
    bool emitted_fneg_mask;
    bool emitted_fabs_mask;
    bool failed;
} x86_mir_emit_t;

/* Every operation names FPR vregs by their assignment; `mem` is an AT&T
 * memory operand such as "8(%ebp)" or "(%eax)". */
struct x86_fp_unit {
    const char *name;
    /* Bytes of private frame area (placed at fp_area_off). */
    int (*frame_bytes)(const x86_mir_emit_t *emit);
    /* FPR <- FPR, and the 32-bit bit moves FPR <-> GPR (BITCAST). */
    void (*copy)(x86_mir_emit_t *emit, anvil_mir_vreg_t dst, anvil_mir_vreg_t src);
    void (*constant)(x86_mir_emit_t *emit, anvil_mir_vreg_t dst, uint64_t bits);
    /* ADD, SUB, MUL, DIV/FDIV. */
    void (*binary)(x86_mir_emit_t *emit, anvil_mir_opcode_t op, anvil_mir_vreg_t dst, anvil_mir_vreg_t a, anvil_mir_vreg_t b);
    /* NEG, FABS. */
    void (*unary)(x86_mir_emit_t *emit, anvil_mir_opcode_t op, anvil_mir_vreg_t dst, anvil_mir_vreg_t src);
    /* dst = MNEMONIC(src): a one-source instruction of the unit named by
     * the scalar table (sqrtsd; fsqrt on ST(0)). */
    void (*apply)(x86_mir_emit_t *emit, const char *mnemonic, anvil_mir_vreg_t dst, anvil_mir_vreg_t src);
    /* Sets ZF/PF/CF as `ucomis b, a` does (unordered: all three). */
    void (*compare)(x86_mir_emit_t *emit, anvil_mir_vreg_t a, anvil_mir_vreg_t b);
    /* SITOFP, UITOFP (32-bit GPR source), FPTOSI, FPTOUI (truncating, into
     * a 32-bit GPR), FPEXT, FPTRUNC. */
    void (*convert)(x86_mir_emit_t *emit, anvil_mir_opcode_t op, anvil_mir_vreg_t dst, anvil_mir_vreg_t src);
    void (*load)(x86_mir_emit_t *emit, anvil_mir_vreg_t dst, const char *mem);
    void (*store)(x86_mir_emit_t *emit, anvil_mir_vreg_t src, const char *mem);
    /* The i386 C conventions return floating values in ST(0). */
    void (*push_result)(x86_mir_emit_t *emit, anvil_mir_vreg_t src);
    void (*pop_result)(x86_mir_emit_t *emit, anvil_mir_vreg_t dst);
};

extern const x86_fp_unit_t x86_fp_sse2_unit;
extern const x86_fp_unit_t x86_fp_x87_unit;

static inline int x86_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0)
        return 4;
    int size = (int)((size_bits + 7) / 8);
    if (size <= 0)
        return 4;
    if (size > 8)
        return 8;
    return size;
}

static inline const anvil_mir_vreg_info_t *x86_vreg_info_checked(x86_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(emit->mir, vreg);
    if (!info)
        emit->failed = true;
    return info;
}

static inline int x86_phys_of(x86_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *assignment = anvil_mir_get_assignment(emit->mir, vreg);
    if (!assignment || assignment->spilled || assignment->phys_reg < 0) {
        emit->failed = true;
        return -1;
    }
    return assignment->phys_reg;
}

static inline const char *x86_gpr_name(int phys_reg, int size)
{
    if (phys_reg < 0 || phys_reg >= 8)
        return "?";
    switch (size) {
    case 1:
        return x86_reg8_names[phys_reg];
    case 2:
        return x86_reg16_names[phys_reg];
    default:
        return x86_reg32_names[phys_reg];
    }
}

/* Width in bytes of an FPR vreg (4 or 8), or 0 after marking failure. */
static inline int x86_fp_size(x86_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = x86_vreg_info_checked(emit, vreg);
    return info ? x86_size_bytes(info->size_bits) : 0;
}

/* reg = the GOT address (position-independent ELF code). */
void x86_emit_got_pointer(x86_mir_emit_t *emit, const char *reg);

#endif
