/* AArch64 Advanced SIMD (NEON) 128-bit instructions as data: the cost hook,
 * MIR legality and the printer all read this table.  Mnemonics take the
 * arrangement of the lane width (.16b .8h .4s .2d) from the printer. */
#include "arm64_internal.h"

#define NEON ANVIL_FEATURE_ARM64_NEON
#define L ANVIL_VECTOR_SHAPE_LANES
#define U ANVIL_VECTOR_SHAPE_UNIFORM
#define INT ANVIL_VECTOR_LANES_INT
#define FP ANVIL_VECTOR_LANES_FP
#define ANY ANVIL_VECTOR_LANES_ANY
#define S ANVIL_VECTOR_SHAPE_SCALAR
#define W2 ANVIL_VECTOR_SHAPE_WIDEN2
#define W4 ANVIL_VECTOR_SHAPE_WIDEN4
#define W8 ANVIL_VECTOR_SHAPE_WIDEN8

const anvil_vector_isa_t arm64_vector_isa[] = {
    {ANVIL_OP_LOAD, 0, ANY, L, NEON, 1, "ldr"},
    {ANVIL_OP_STORE, 0, ANY, L, NEON, 1, "str"},

    {ANVIL_OP_FADD, 32, FP, L, NEON, 1, "fadd"},
    {ANVIL_OP_FADD, 64, FP, L, NEON, 1, "fadd"},
    {ANVIL_OP_FSUB, 32, FP, L, NEON, 1, "fsub"},
    {ANVIL_OP_FSUB, 64, FP, L, NEON, 1, "fsub"},
    {ANVIL_OP_FMUL, 32, FP, L, NEON, 1, "fmul"},
    {ANVIL_OP_FMUL, 64, FP, L, NEON, 1, "fmul"},
    {ANVIL_OP_FDIV, 32, FP, L, NEON, 4, "fdiv"},
    {ANVIL_OP_FDIV, 64, FP, L, NEON, 4, "fdiv"},
    /* fmla accumulates into its destination; fmadd takes four registers. */
    {ANVIL_OP_FMA, 32, FP, L, NEON, 1, "fmla"},
    {ANVIL_OP_FMA, 64, FP, L, NEON, 1, "fmla"},
    {ANVIL_OP_FMA, 32, FP, S, 0, 1, "fmadd"},
    {ANVIL_OP_FMA, 64, FP, S, 0, 1, "fmadd"},
    {ANVIL_OP_FSQRT, 32, FP, L, NEON, 4, "fsqrt"},
    {ANVIL_OP_FSQRT, 64, FP, L, NEON, 4, "fsqrt"},
    {ANVIL_OP_FSQRT, 32, FP, S, 0, 4, "fsqrt"},
    {ANVIL_OP_FSQRT, 64, FP, S, 0, 4, "fsqrt"},
    /* Rotate left as ror by the negated count. */
    {ANVIL_OP_ROTL, 32, INT, S, 0, 2, "ror"},
    {ANVIL_OP_ROTL, 64, INT, S, 0, 2, "ror"},
    /* High half of a product; 32-bit takes the 64-bit product's upper
     * word (umull/smull, then lsr #32). */
    {ANVIL_OP_UMULH, 32, INT, S, 0, 2, "umull"},
    {ANVIL_OP_UMULH, 64, INT, S, 0, 2, "umulh"},
    {ANVIL_OP_SMULH, 32, INT, S, 0, 2, "smull"},
    {ANVIL_OP_SMULH, 64, INT, S, 0, 2, "smulh"},

    /* dup: lane 0 of an FP register, or a general register. */
    {ANVIL_OP_SPLAT, 32, FP, L, NEON, 1, "dup"},
    {ANVIL_OP_SPLAT, 64, FP, L, NEON, 1, "dup"},
    {ANVIL_OP_SPLAT, 0, INT, L, NEON, 1, "dup"},

    {ANVIL_OP_ADD, 0, INT, L, NEON, 1, "add"},
    {ANVIL_OP_SUB, 0, INT, L, NEON, 1, "sub"},
    {ANVIL_OP_MUL, 8, INT, L, NEON, 1, "mul"},
    {ANVIL_OP_MUL, 16, INT, L, NEON, 1, "mul"},
    {ANVIL_OP_MUL, 32, INT, L, NEON, 1, "mul"},
    /* Saturating arithmetic, every lane width. */
    {ANVIL_OP_ADD_SAT_U, 0, INT, L, NEON, 1, "uqadd"},
    {ANVIL_OP_ADD_SAT_S, 0, INT, L, NEON, 1, "sqadd"},
    {ANVIL_OP_SUB_SAT_U, 0, INT, L, NEON, 1, "uqsub"},
    {ANVIL_OP_SUB_SAT_S, 0, INT, L, NEON, 1, "sqsub"},
    {ANVIL_OP_AND, 0, INT, L, NEON, 1, "and"},
    {ANVIL_OP_OR, 0, INT, L, NEON, 1, "orr"},
    {ANVIL_OP_XOR, 0, INT, L, NEON, 1, "eor"},

    /* Register shifts go left by a signed per-lane count; the printer
     * negates the count for right shifts (and dups a uniform one). */
    {ANVIL_OP_SHL, 0, INT, L, NEON, 1, "ushl"},
    {ANVIL_OP_SHR, 0, INT, L, NEON, 2, "ushl"},
    {ANVIL_OP_SAR, 0, INT, L, NEON, 2, "sshl"},
    {ANVIL_OP_SHL, 0, INT, U, NEON, 2, "ushl"},
    {ANVIL_OP_SHR, 0, INT, U, NEON, 2, "ushl"},
    {ANVIL_OP_SAR, 0, INT, U, NEON, 2, "sshl"},

    /* Lane compares produce all-ones / zero masks; the printer swaps
     * operands for < and <=, and complements (mvn) for != and the
     * unordered FP predicates. */
    {ANVIL_OP_CMP_EQ, 0, INT, L, NEON, 1, "cmeq"},
    {ANVIL_OP_CMP_NE, 0, INT, L, NEON, 2, "cmeq"},
    {ANVIL_OP_CMP_GT, 0, INT, L, NEON, 1, "cmgt"},
    {ANVIL_OP_CMP_LT, 0, INT, L, NEON, 1, "cmgt"},
    {ANVIL_OP_CMP_GE, 0, INT, L, NEON, 1, "cmge"},
    {ANVIL_OP_CMP_LE, 0, INT, L, NEON, 1, "cmge"},
    {ANVIL_OP_CMP_UGT, 0, INT, L, NEON, 1, "cmhi"},
    {ANVIL_OP_CMP_ULT, 0, INT, L, NEON, 1, "cmhi"},
    {ANVIL_OP_CMP_UGE, 0, INT, L, NEON, 1, "cmhs"},
    {ANVIL_OP_CMP_ULE, 0, INT, L, NEON, 1, "cmhs"},
    {ANVIL_OP_FCMP, 32, FP, L, NEON, 2, "fcm"},
    {ANVIL_OP_FCMP, 64, FP, L, NEON, 2, "fcm"},

    /* bsl/bit/bif, whichever keeps the inputs the destination aliases. */
    {ANVIL_OP_SELECT, 0, ANY, L, NEON, 1, "bsl"},

    /* Reductions: across-lane instructions (addv, smaxv...), pairwise
     * faddp for FP, and for the bitwise ones (no across-lane form) halves
     * folded with ext and the operation named.  REDUCE_OR's cost is that of
     * a lane mask (umaxv), the form every vectorized exit test uses. */
    {ANVIL_OP_REDUCE_OR, 0, INT, L, NEON, 2, "orr"},
    {ANVIL_OP_REDUCE_ADD, 8, INT, L, NEON, 2, "addv"},
    {ANVIL_OP_REDUCE_ADD, 16, INT, L, NEON, 2, "addv"},
    {ANVIL_OP_REDUCE_ADD, 32, INT, L, NEON, 2, "addv"},
    {ANVIL_OP_REDUCE_ADD, 64, INT, L, NEON, 2, "addp"},
    {ANVIL_OP_REDUCE_SMAX, 8, INT, L, NEON, 2, "smaxv"},
    {ANVIL_OP_REDUCE_SMAX, 16, INT, L, NEON, 2, "smaxv"},
    {ANVIL_OP_REDUCE_SMAX, 32, INT, L, NEON, 2, "smaxv"},
    {ANVIL_OP_REDUCE_SMIN, 8, INT, L, NEON, 2, "sminv"},
    {ANVIL_OP_REDUCE_SMIN, 16, INT, L, NEON, 2, "sminv"},
    {ANVIL_OP_REDUCE_SMIN, 32, INT, L, NEON, 2, "sminv"},
    {ANVIL_OP_REDUCE_UMAX, 8, INT, L, NEON, 2, "umaxv"},
    {ANVIL_OP_REDUCE_UMAX, 16, INT, L, NEON, 2, "umaxv"},
    {ANVIL_OP_REDUCE_UMAX, 32, INT, L, NEON, 2, "umaxv"},
    {ANVIL_OP_REDUCE_UMIN, 8, INT, L, NEON, 2, "uminv"},
    {ANVIL_OP_REDUCE_UMIN, 16, INT, L, NEON, 2, "uminv"},
    {ANVIL_OP_REDUCE_UMIN, 32, INT, L, NEON, 2, "uminv"},
    {ANVIL_OP_REDUCE_AND, 8, INT, L, NEON, 6, "and"},
    {ANVIL_OP_REDUCE_AND, 16, INT, L, NEON, 5, "and"},
    {ANVIL_OP_REDUCE_AND, 32, INT, L, NEON, 4, "and"},
    {ANVIL_OP_REDUCE_AND, 64, INT, L, NEON, 3, "and"},
    {ANVIL_OP_REDUCE_XOR, 8, INT, L, NEON, 6, "eor"},
    {ANVIL_OP_REDUCE_XOR, 16, INT, L, NEON, 5, "eor"},
    {ANVIL_OP_REDUCE_XOR, 32, INT, L, NEON, 4, "eor"},
    {ANVIL_OP_REDUCE_XOR, 64, INT, L, NEON, 3, "eor"},
    {ANVIL_OP_REDUCE_FADD, 32, FP, L, NEON, 2, "faddp"},
    {ANVIL_OP_REDUCE_FADD, 64, FP, L, NEON, 1, "faddp"},

    /* Narrow lanes summed into wider accumulator lanes (lane width is the
     * accumulator's): pairwise widening adds (uaddlp/saddlp) into the
     * temporary until one doubling remains, which uadalp/sadalp
     * accumulates. */
    {ANVIL_OP_ACCUM_ZEXT, 16, INT, W2, NEON, 1, "uadalp"},
    {ANVIL_OP_ACCUM_ZEXT, 32, INT, W2, NEON, 1, "uadalp"},
    {ANVIL_OP_ACCUM_ZEXT, 64, INT, W2, NEON, 1, "uadalp"},
    {ANVIL_OP_ACCUM_ZEXT, 32, INT, W4, NEON, 2, "uadalp"},
    {ANVIL_OP_ACCUM_ZEXT, 64, INT, W4, NEON, 2, "uadalp"},
    {ANVIL_OP_ACCUM_ZEXT, 64, INT, W8, NEON, 3, "uadalp"},
    {ANVIL_OP_ACCUM_SEXT, 16, INT, W2, NEON, 1, "sadalp"},
    {ANVIL_OP_ACCUM_SEXT, 32, INT, W2, NEON, 1, "sadalp"},
    {ANVIL_OP_ACCUM_SEXT, 64, INT, W2, NEON, 1, "sadalp"},
    {ANVIL_OP_ACCUM_SEXT, 32, INT, W4, NEON, 2, "sadalp"},
    {ANVIL_OP_ACCUM_SEXT, 64, INT, W4, NEON, 2, "sadalp"},
    {ANVIL_OP_ACCUM_SEXT, 64, INT, W8, NEON, 3, "sadalp"},
};

const size_t arm64_vector_isa_count = sizeof(arm64_vector_isa) / sizeof(arm64_vector_isa[0]);

unsigned arm64_vector_operation_cost(anvil_backend_t *be, anvil_op_t operation, anvil_type_t *type, anvil_vector_shape_t shape)
{
    return anvil_vector_isa_cost(arm64_vector_isa, arm64_vector_isa_count, 16, be->ctx, operation, type, shape);
}
