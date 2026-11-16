/* State shared by the i386 GAS printer (gas32.c) and its floating-point
 * units: SSE2 (gas32_sse2.c) and the x87 FPU (gas32_x87.c).  The unit is
 * chosen from the CPU model's features; MachineIR is the same for both. */
#ifndef ANVIL_X86_GAS32_INTERNAL_H
#define ANVIL_X86_GAS32_INTERNAL_H

#include "../x86_32_internal.h"
#include "x86_gas.h"
#include "../../common/print_flags.h"
#include "../../common/mir_isel.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct x86_fp_unit x86_fp_unit_t;

typedef struct {
    x86_gas_t base;
    const anvil_func_t *source_func;
    const anvil_x86_cc_desc_t *desc;
    const anvil_x86_plat_desc_t *plat;
    const x86_fp_unit_t *fp;
    anvil_cpu_features_t features;
    anvil_reloc_model_t reloc;      /* STATIC, or position-independent (ELF) */
    unsigned pic_labels;
    int gpr_save_offsets[8];
    int scratch_a_off;
    int scratch_b_off;  /* 8 bytes; its high word is scratch_a */
    int fp_area_off;    /* the FP unit's frame area, at -fp_area_off(%ebp) */
    int num_pushes;     /* callee-saved registers pushed (no frame pointer) */
    /* Without a frame pointer EBP is allocated and the frame is addressed
     * from ESP, which stays frame_size bytes below where EBP would be
     * (the prologue reserves EBP's push slot too). */
    bool frame_pointer;
    bool fp_returns;
    /* RET_VALUE_PART's register, moved to EDX together with RET's EAX. */
    int ret_hi_phys;
    /* The CALL already moved EDX into the following CALL_RESULT. */
    bool call_hi_moved;
    /* Floating constants the function reads from its literal pool
     * (position-dependent code), `size` bytes of `bits` each. */
    struct {
        uint64_t bits;
        int size;
    } *fp_pool;
    size_t fp_pool_count, fp_pool_cap;
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
    if (size == 16)
        return 16;  /* a vector in an XMM register */
    if (size > 8)
        return 8;
    return size;
}

static inline const anvil_mir_vreg_info_t *x86_vreg_info_checked(x86_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(emit->base.mir, vreg);
    if (!info)
        emit->base.failed = true;
    return info;
}

/* Width in bytes of an FPR vreg (4, 8, or 16 for a vector), or 0 after
 * marking failure. */
static inline int x86_fp_size(x86_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = x86_vreg_info_checked(emit, vreg);
    return info ? x86_size_bytes(info->size_bits) : 0;
}

/* reg = the GOT address (position-independent ELF code). */
void x86_emit_got_pointer(x86_mir_emit_t *emit, const char *reg);

#endif
