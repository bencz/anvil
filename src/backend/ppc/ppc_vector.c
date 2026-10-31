/* PowerPC 128-bit vector instructions as data: the cost hook, MIR legality
 * and the printer all read this table.  Vectors live in the AltiVec
 * registers (VSX 32-63): VSX instructions (mnemonics starting with 'x') name
 * them vs32-vs63, AltiVec ones ('v') v0-v31.  lxvd2x/stxvd2x keep every
 * aligned lane whole on both endiannesses, so lane-wise operations are
 * exact. */
#include "ppc_internal.h"

#define VMX ANVIL_FEATURE_PPC_ALTIVEC
#define VSX ANVIL_FEATURE_PPC_VSX
#define FSQRT ANVIL_FEATURE_PPC_FSQRT
#define P8 (ANVIL_FEATURE_PPC_VSX | ANVIL_FEATURE_PPC_POWER8_VEC)
#define L ANVIL_VECTOR_SHAPE_LANES
#define INT ANVIL_VECTOR_LANES_INT
#define FP ANVIL_VECTOR_LANES_FP
#define ANY ANVIL_VECTOR_LANES_ANY
#define S ANVIL_VECTOR_SHAPE_SCALAR
#define W2 ANVIL_VECTOR_SHAPE_WIDEN2
#define W4 ANVIL_VECTOR_SHAPE_WIDEN4

const anvil_vector_isa_t ppc_vector_isa[] = {
    {ANVIL_OP_LOAD, 0, ANY, L, VSX, 1, "lxvd2x"},
    {ANVIL_OP_STORE, 0, ANY, L, VSX, 1, "stxvd2x"},

    {ANVIL_OP_FADD, 32, FP, L, VSX, 1, "xvaddsp"},
    {ANVIL_OP_FADD, 64, FP, L, VSX, 1, "xvadddp"},
    {ANVIL_OP_FSUB, 32, FP, L, VSX, 1, "xvsubsp"},
    {ANVIL_OP_FSUB, 64, FP, L, VSX, 1, "xvsubdp"},
    {ANVIL_OP_FMUL, 32, FP, L, VSX, 1, "xvmulsp"},
    {ANVIL_OP_FMUL, 64, FP, L, VSX, 1, "xvmuldp"},
    {ANVIL_OP_FDIV, 32, FP, L, VSX, 4, "xvdivsp"},
    {ANVIL_OP_FDIV, 64, FP, L, VSX, 4, "xvdivdp"},
    /* xvmadda accumulates into its target; fmadd(s) takes four registers. */
    {ANVIL_OP_FMA, 32, FP, L, VSX, 1, "xvmaddasp"},
    {ANVIL_OP_FMA, 64, FP, L, VSX, 1, "xvmaddadp"},
    {ANVIL_OP_FMA, 32, FP, S, 0, 1, "fmadds"},
    {ANVIL_OP_FMA, 64, FP, S, 0, 1, "fmadd"},
    /* fsqrt(s) is an optional instruction (absent on the G3 and G4). */
    {ANVIL_OP_FSQRT, 32, FP, L, VSX, 4, "xvsqrtsp"},
    {ANVIL_OP_FSQRT, 64, FP, L, VSX, 4, "xvsqrtdp"},
    {ANVIL_OP_FSQRT, 32, FP, S, FSQRT, 4, "fsqrts"},
    {ANVIL_OP_FSQRT, 64, FP, S, FSQRT, 4, "fsqrt"},
    /* High half of a word product (the doubleword forms are 64-bit mode
     * only, below). */
    {ANVIL_OP_ROTL, 32, INT, S, 0, 1, "rotlw"},
    {ANVIL_OP_UMULH, 32, INT, S, 0, 2, "mulhwu"},
    {ANVIL_OP_SMULH, 32, INT, S, 0, 2, "mulhw"},

    /* Broadcasts: an FPR scalar is doubleword 0 of its VSR in double
     * format; a GPR moves in directly (POWER8) and splats from v0. */
    {ANVIL_OP_SPLAT, 32, FP, L, VSX, 2, "xxspltw"},
    {ANVIL_OP_SPLAT, 64, FP, L, VSX, 1, "xxpermdi"},
    {ANVIL_OP_SPLAT, 8, INT, L, P8, 2, "vspltb"},
    {ANVIL_OP_SPLAT, 16, INT, L, P8, 2, "vsplth"},
    {ANVIL_OP_SPLAT, 32, INT, L, P8, 2, "xxspltw"},

    {ANVIL_OP_ADD, 8, INT, L, VSX, 1, "vaddubm"},
    {ANVIL_OP_ADD, 16, INT, L, VSX, 1, "vadduhm"},
    {ANVIL_OP_ADD, 32, INT, L, VSX, 1, "vadduwm"},
    {ANVIL_OP_ADD, 64, INT, L, P8, 1, "vaddudm"},
    {ANVIL_OP_SUB, 8, INT, L, VSX, 1, "vsububm"},
    {ANVIL_OP_SUB, 16, INT, L, VSX, 1, "vsubuhm"},
    {ANVIL_OP_SUB, 32, INT, L, VSX, 1, "vsubuwm"},
    {ANVIL_OP_SUB, 64, INT, L, P8, 1, "vsubudm"},
    /* Halfword products add a zero vector the printer builds in v0. */
    {ANVIL_OP_MUL, 16, INT, L, VSX, 2, "vmladduhm"},
    {ANVIL_OP_MUL, 32, INT, L, P8, 2, "vmuluwm"},
    /* Saturating arithmetic on bytes, halfwords and words. */
    {ANVIL_OP_ADD_SAT_U, 8, INT, L, VSX, 1, "vaddubs"},
    {ANVIL_OP_ADD_SAT_U, 16, INT, L, VSX, 1, "vadduhs"},
    {ANVIL_OP_ADD_SAT_U, 32, INT, L, VSX, 1, "vadduws"},
    {ANVIL_OP_ADD_SAT_S, 8, INT, L, VSX, 1, "vaddsbs"},
    {ANVIL_OP_ADD_SAT_S, 16, INT, L, VSX, 1, "vaddshs"},
    {ANVIL_OP_ADD_SAT_S, 32, INT, L, VSX, 1, "vaddsws"},
    {ANVIL_OP_SUB_SAT_U, 8, INT, L, VSX, 1, "vsububs"},
    {ANVIL_OP_SUB_SAT_U, 16, INT, L, VSX, 1, "vsubuhs"},
    {ANVIL_OP_SUB_SAT_U, 32, INT, L, VSX, 1, "vsubuws"},
    {ANVIL_OP_SUB_SAT_S, 8, INT, L, VSX, 1, "vsubsbs"},
    {ANVIL_OP_SUB_SAT_S, 16, INT, L, VSX, 1, "vsubshs"},
    {ANVIL_OP_SUB_SAT_S, 32, INT, L, VSX, 1, "vsubsws"},
    {ANVIL_OP_AND, 0, INT, L, VSX, 1, "vand"},
    {ANVIL_OP_OR, 0, INT, L, VSX, 1, "vor"},
    {ANVIL_OP_XOR, 0, INT, L, VSX, 1, "vxor"},

    /* Per-lane counts (modulo the lane width). */
    {ANVIL_OP_SHL, 8, INT, L, VSX, 1, "vslb"},
    {ANVIL_OP_SHL, 16, INT, L, VSX, 1, "vslh"},
    {ANVIL_OP_SHL, 32, INT, L, VSX, 1, "vslw"},
    {ANVIL_OP_SHL, 64, INT, L, P8, 1, "vsld"},
    {ANVIL_OP_SHR, 8, INT, L, VSX, 1, "vsrb"},
    {ANVIL_OP_SHR, 16, INT, L, VSX, 1, "vsrh"},
    {ANVIL_OP_SHR, 32, INT, L, VSX, 1, "vsrw"},
    {ANVIL_OP_SHR, 64, INT, L, P8, 1, "vsrd"},
    {ANVIL_OP_SAR, 8, INT, L, VSX, 1, "vsrab"},
    {ANVIL_OP_SAR, 16, INT, L, VSX, 1, "vsrah"},
    {ANVIL_OP_SAR, 32, INT, L, VSX, 1, "vsraw"},
    {ANVIL_OP_SAR, 64, INT, L, P8, 1, "vsrad"},

    /* Lane compares produce all-ones / zero masks: the printer swaps
     * operands for < and complements (vnor/xxlnor) for !=, >=, <= and the
     * unordered FP predicates. */
    {ANVIL_OP_CMP_EQ, 8, INT, L, VSX, 1, "vcmpequb"},
    {ANVIL_OP_CMP_EQ, 16, INT, L, VSX, 1, "vcmpequh"},
    {ANVIL_OP_CMP_EQ, 32, INT, L, VSX, 1, "vcmpequw"},
    {ANVIL_OP_CMP_EQ, 64, INT, L, P8, 1, "vcmpequd"},
    {ANVIL_OP_CMP_NE, 8, INT, L, VSX, 2, "vcmpequb"},
    {ANVIL_OP_CMP_NE, 16, INT, L, VSX, 2, "vcmpequh"},
    {ANVIL_OP_CMP_NE, 32, INT, L, VSX, 2, "vcmpequw"},
    {ANVIL_OP_CMP_NE, 64, INT, L, P8, 2, "vcmpequd"},
    {ANVIL_OP_CMP_GT, 8, INT, L, VSX, 1, "vcmpgtsb"},
    {ANVIL_OP_CMP_GT, 16, INT, L, VSX, 1, "vcmpgtsh"},
    {ANVIL_OP_CMP_GT, 32, INT, L, VSX, 1, "vcmpgtsw"},
    {ANVIL_OP_CMP_GT, 64, INT, L, P8, 1, "vcmpgtsd"},
    {ANVIL_OP_CMP_LT, 8, INT, L, VSX, 1, "vcmpgtsb"},
    {ANVIL_OP_CMP_LT, 16, INT, L, VSX, 1, "vcmpgtsh"},
    {ANVIL_OP_CMP_LT, 32, INT, L, VSX, 1, "vcmpgtsw"},
    {ANVIL_OP_CMP_LT, 64, INT, L, P8, 1, "vcmpgtsd"},
    {ANVIL_OP_CMP_GE, 8, INT, L, VSX, 2, "vcmpgtsb"},
    {ANVIL_OP_CMP_GE, 16, INT, L, VSX, 2, "vcmpgtsh"},
    {ANVIL_OP_CMP_GE, 32, INT, L, VSX, 2, "vcmpgtsw"},
    {ANVIL_OP_CMP_GE, 64, INT, L, P8, 2, "vcmpgtsd"},
    {ANVIL_OP_CMP_LE, 8, INT, L, VSX, 2, "vcmpgtsb"},
    {ANVIL_OP_CMP_LE, 16, INT, L, VSX, 2, "vcmpgtsh"},
    {ANVIL_OP_CMP_LE, 32, INT, L, VSX, 2, "vcmpgtsw"},
    {ANVIL_OP_CMP_LE, 64, INT, L, P8, 2, "vcmpgtsd"},
    {ANVIL_OP_CMP_UGT, 8, INT, L, VSX, 1, "vcmpgtub"},
    {ANVIL_OP_CMP_UGT, 16, INT, L, VSX, 1, "vcmpgtuh"},
    {ANVIL_OP_CMP_UGT, 32, INT, L, VSX, 1, "vcmpgtuw"},
    {ANVIL_OP_CMP_UGT, 64, INT, L, P8, 1, "vcmpgtud"},
    {ANVIL_OP_CMP_ULT, 8, INT, L, VSX, 1, "vcmpgtub"},
    {ANVIL_OP_CMP_ULT, 16, INT, L, VSX, 1, "vcmpgtuh"},
    {ANVIL_OP_CMP_ULT, 32, INT, L, VSX, 1, "vcmpgtuw"},
    {ANVIL_OP_CMP_ULT, 64, INT, L, P8, 1, "vcmpgtud"},
    {ANVIL_OP_CMP_UGE, 8, INT, L, VSX, 2, "vcmpgtub"},
    {ANVIL_OP_CMP_UGE, 16, INT, L, VSX, 2, "vcmpgtuh"},
    {ANVIL_OP_CMP_UGE, 32, INT, L, VSX, 2, "vcmpgtuw"},
    {ANVIL_OP_CMP_UGE, 64, INT, L, P8, 2, "vcmpgtud"},
    {ANVIL_OP_CMP_ULE, 8, INT, L, VSX, 2, "vcmpgtub"},
    {ANVIL_OP_CMP_ULE, 16, INT, L, VSX, 2, "vcmpgtuh"},
    {ANVIL_OP_CMP_ULE, 32, INT, L, VSX, 2, "vcmpgtuw"},
    {ANVIL_OP_CMP_ULE, 64, INT, L, P8, 2, "vcmpgtud"},
    {ANVIL_OP_FCMP, 32, FP, L, VSX, 2, "xvcmp*sp"},
    {ANVIL_OP_FCMP, 64, FP, L, VSX, 2, "xvcmp*dp"},

    {ANVIL_OP_SELECT, 0, ANY, L, VSX, 1, "xxsel"},

    /* Narrow lanes summed into word lanes: vsum4ubs/vsum4sbs/vsum4shs
     * against zero (one vector's group sums never saturate), then a
     * wrapping vadduwm into the accumulator. */
    {ANVIL_OP_ACCUM_ZEXT, 32, INT, W4, VSX, 3, "vsum4ubs"},
    {ANVIL_OP_ACCUM_SEXT, 32, INT, W4, VSX, 3, "vsum4sbs"},
    {ANVIL_OP_ACCUM_SEXT, 32, INT, W2, VSX, 3, "vsum4shs"},

    /* 64-bit mode only: these work on whole doubleword general registers
     * (a 32-bit ABI keeps only their low words).  ppc32 stops before. */
    {ANVIL_OP_SPLAT, 64, INT, L, P8, 2, "xxpermdi"},
    /* Doublewords folded with xxlor, moved out (mfvsrd), then folded in a
     * general register.  Cost of the lane-mask form every vectorized test
     * uses. */
    {ANVIL_OP_REDUCE_OR, 0, INT, L, P8, 2, "xxlor"},
    /* Reductions rotate the whole vector and combine (xxpermdi, then
     * vsldoi by 4, 2, 1 bytes), so every lane ends with the result and the
     * moved-out doubleword holds it on either endianness. */
    {ANVIL_OP_REDUCE_ADD, 8, INT, L, P8, 5, "vaddubm"},
    {ANVIL_OP_REDUCE_ADD, 16, INT, L, P8, 4, "vadduhm"},
    {ANVIL_OP_REDUCE_ADD, 32, INT, L, P8, 3, "vadduwm"},
    {ANVIL_OP_REDUCE_ADD, 64, INT, L, P8, 2, "vaddudm"},
    {ANVIL_OP_REDUCE_SMAX, 8, INT, L, P8, 5, "vmaxsb"},
    {ANVIL_OP_REDUCE_SMAX, 16, INT, L, P8, 4, "vmaxsh"},
    {ANVIL_OP_REDUCE_SMAX, 32, INT, L, P8, 3, "vmaxsw"},
    {ANVIL_OP_REDUCE_SMAX, 64, INT, L, P8, 2, "vmaxsd"},
    {ANVIL_OP_REDUCE_SMIN, 8, INT, L, P8, 5, "vminsb"},
    {ANVIL_OP_REDUCE_SMIN, 16, INT, L, P8, 4, "vminsh"},
    {ANVIL_OP_REDUCE_SMIN, 32, INT, L, P8, 3, "vminsw"},
    {ANVIL_OP_REDUCE_SMIN, 64, INT, L, P8, 2, "vminsd"},
    {ANVIL_OP_REDUCE_UMAX, 8, INT, L, P8, 5, "vmaxub"},
    {ANVIL_OP_REDUCE_UMAX, 16, INT, L, P8, 4, "vmaxuh"},
    {ANVIL_OP_REDUCE_UMAX, 32, INT, L, P8, 3, "vmaxuw"},
    {ANVIL_OP_REDUCE_UMAX, 64, INT, L, P8, 2, "vmaxud"},
    {ANVIL_OP_REDUCE_UMIN, 8, INT, L, P8, 5, "vminub"},
    {ANVIL_OP_REDUCE_UMIN, 16, INT, L, P8, 4, "vminuh"},
    {ANVIL_OP_REDUCE_UMIN, 32, INT, L, P8, 3, "vminuw"},
    {ANVIL_OP_REDUCE_UMIN, 64, INT, L, P8, 2, "vminud"},
    {ANVIL_OP_REDUCE_AND, 8, INT, L, P8, 5, "vand"},
    {ANVIL_OP_REDUCE_AND, 16, INT, L, P8, 4, "vand"},
    {ANVIL_OP_REDUCE_AND, 32, INT, L, P8, 3, "vand"},
    {ANVIL_OP_REDUCE_AND, 64, INT, L, P8, 2, "vand"},
    {ANVIL_OP_REDUCE_XOR, 8, INT, L, P8, 5, "vxor"},
    {ANVIL_OP_REDUCE_XOR, 16, INT, L, P8, 4, "vxor"},
    {ANVIL_OP_REDUCE_XOR, 32, INT, L, P8, 3, "vxor"},
    {ANVIL_OP_REDUCE_XOR, 64, INT, L, P8, 2, "vxor"},
    {ANVIL_OP_REDUCE_FADD, 32, FP, L, P8, 3, "xvaddsp"},
    {ANVIL_OP_REDUCE_FADD, 64, FP, L, P8, 2, "xvadddp"},
    {ANVIL_OP_UMULH, 64, INT, S, 0, 2, "mulhdu"},
    {ANVIL_OP_SMULH, 64, INT, S, 0, 2, "mulhd"},
    {ANVIL_OP_ROTL, 64, INT, S, 0, 1, "rotld"},
};

/* Splat, REDUCE_OR, the 30 reductions, the two doubleword products and
 * the doubleword rotate. */
enum { PPC_VECTOR_64BIT_ENTRIES = 2 + 30 + 2 + 1 };

size_t ppc_vector_isa_count(const anvil_ppc_target_desc_t *desc)
{
    size_t all = sizeof(ppc_vector_isa) / sizeof(ppc_vector_isa[0]);
    return desc && desc->word_size == 8 ? all : all - PPC_VECTOR_64BIT_ENTRIES;
}

unsigned anvil_ppc_backend_vector_operation_cost(anvil_backend_t *be, anvil_op_t operation, anvil_type_t *type, anvil_vector_shape_t shape)
{
    return anvil_vector_isa_cost(ppc_vector_isa, ppc_vector_isa_count(ppc_backend_desc(be)), 16, be->ctx, operation, type, shape);
}
