/* x86-64 128-bit vector instructions (SSE through SSE4.1) as data: the cost
 * hook, MIR legality and the printer all read this table. */
#include "x86_64_internal.h"

#define SSE ANVIL_FEATURE_X86_SSE
#define SSE2 ANVIL_FEATURE_X86_SSE2
#define SSSE3 ANVIL_FEATURE_X86_SSSE3
#define SSE41 ANVIL_FEATURE_X86_SSE41
#define SSE42 ANVIL_FEATURE_X86_SSE42
#define FMA3 (ANVIL_FEATURE_X86_FMA | ANVIL_FEATURE_X86_AVX)
#define S ANVIL_VECTOR_SHAPE_SCALAR
#define L ANVIL_VECTOR_SHAPE_LANES
#define U ANVIL_VECTOR_SHAPE_UNIFORM
#define W2 ANVIL_VECTOR_SHAPE_WIDEN2
#define W4 ANVIL_VECTOR_SHAPE_WIDEN4
#define W8 ANVIL_VECTOR_SHAPE_WIDEN8
#define INT ANVIL_VECTOR_LANES_INT
#define FP ANVIL_VECTOR_LANES_FP
#define ANY ANVIL_VECTOR_LANES_ANY

const anvil_vector_isa_t x64_vector_isa[] = {
    {ANVIL_OP_LOAD, 0, ANY, L, SSE2, 1, "movups"},
    {ANVIL_OP_STORE, 0, ANY, L, SSE2, 1, "movups"},

    {ANVIL_OP_FADD, 32, FP, L, SSE, 1, "addps"},
    {ANVIL_OP_FADD, 64, FP, L, SSE2, 1, "addpd"},
    {ANVIL_OP_FSUB, 32, FP, L, SSE, 1, "subps"},
    {ANVIL_OP_FSUB, 64, FP, L, SSE2, 1, "subpd"},
    {ANVIL_OP_FMUL, 32, FP, L, SSE, 1, "mulps"},
    {ANVIL_OP_FMUL, 64, FP, L, SSE2, 1, "mulpd"},
    {ANVIL_OP_FDIV, 32, FP, L, SSE, 4, "divps"},
    {ANVIL_OP_FDIV, 64, FP, L, SSE2, 4, "divpd"},
    /* dst = a * b + dst (VEX encoded; the printer puts c in dst). */
    {ANVIL_OP_FMA, 32, FP, L, FMA3, 1, "vfmadd231ps"},
    {ANVIL_OP_FMA, 64, FP, L, FMA3, 1, "vfmadd231pd"},
    {ANVIL_OP_FMA, 32, FP, S, FMA3, 1, "vfmadd231ss"},
    {ANVIL_OP_FMA, 64, FP, S, FMA3, 1, "vfmadd231sd"},
    /* Square roots cost about a divide. */
    {ANVIL_OP_FSQRT, 32, FP, L, SSE, 4, "sqrtps"},
    {ANVIL_OP_FSQRT, 64, FP, L, SSE2, 4, "sqrtpd"},
    {ANVIL_OP_FSQRT, 32, FP, S, SSE, 4, "sqrtss"},
    {ANVIL_OP_FSQRT, 64, FP, S, SSE2, 4, "sqrtsd"},
    /* Rotates: by an immediate, or by CL (an emitter temporary). */
    {ANVIL_OP_ROTL, 32, INT, S, 0, 1, "roll"},
    {ANVIL_OP_ROTL, 64, INT, S, 0, 1, "rolq"},
    /* High half of a product: one-operand mul/imul into rdx:rax. */
    {ANVIL_OP_UMULH, 32, INT, S, 0, 3, "mull"},
    {ANVIL_OP_UMULH, 64, INT, S, 0, 3, "mulq"},
    {ANVIL_OP_SMULH, 32, INT, S, 0, 3, "imull"},
    {ANVIL_OP_SMULH, 64, INT, S, 0, 3, "imulq"},

    /* Broadcasts; the printer expands the integer ones from a GPR. */
    {ANVIL_OP_SPLAT, 32, FP, L, SSE, 1, "shufps"},
    {ANVIL_OP_SPLAT, 64, FP, L, SSE2, 1, "unpcklpd"},
    {ANVIL_OP_SPLAT, 8, INT, L, SSE2, 3, "punpcklbw"},
    {ANVIL_OP_SPLAT, 16, INT, L, SSE2, 2, "pshuflw"},
    {ANVIL_OP_SPLAT, 32, INT, L, SSE2, 2, "pshufd"},
    {ANVIL_OP_SPLAT, 64, INT, L, SSE2, 2, "punpcklqdq"},

    {ANVIL_OP_ADD, 8, INT, L, SSE2, 1, "paddb"},
    {ANVIL_OP_ADD, 16, INT, L, SSE2, 1, "paddw"},
    {ANVIL_OP_ADD, 32, INT, L, SSE2, 1, "paddd"},
    {ANVIL_OP_ADD, 64, INT, L, SSE2, 1, "paddq"},
    {ANVIL_OP_SUB, 8, INT, L, SSE2, 1, "psubb"},
    {ANVIL_OP_SUB, 16, INT, L, SSE2, 1, "psubw"},
    {ANVIL_OP_SUB, 32, INT, L, SSE2, 1, "psubd"},
    {ANVIL_OP_SUB, 64, INT, L, SSE2, 1, "psubq"},
    {ANVIL_OP_MUL, 16, INT, L, SSE2, 1, "pmullw"},
    {ANVIL_OP_MUL, 32, INT, L, SSE41, 2, "pmulld"},
    /* Saturating byte and word arithmetic. */
    {ANVIL_OP_ADD_SAT_U, 8, INT, L, SSE2, 1, "paddusb"},
    {ANVIL_OP_ADD_SAT_U, 16, INT, L, SSE2, 1, "paddusw"},
    {ANVIL_OP_ADD_SAT_S, 8, INT, L, SSE2, 1, "paddsb"},
    {ANVIL_OP_ADD_SAT_S, 16, INT, L, SSE2, 1, "paddsw"},
    {ANVIL_OP_SUB_SAT_U, 8, INT, L, SSE2, 1, "psubusb"},
    {ANVIL_OP_SUB_SAT_U, 16, INT, L, SSE2, 1, "psubusw"},
    {ANVIL_OP_SUB_SAT_S, 8, INT, L, SSE2, 1, "psubsb"},
    {ANVIL_OP_SUB_SAT_S, 16, INT, L, SSE2, 1, "psubsw"},
    {ANVIL_OP_AND, 0, INT, L, SSE2, 1, "pand"},
    {ANVIL_OP_OR, 0, INT, L, SSE2, 1, "por"},
    {ANVIL_OP_XOR, 0, INT, L, SSE2, 1, "pxor"},

    /* SSE shifts take one count for every lane (no byte shifts, no 64-bit
     * arithmetic shift before AVX-512). */
    {ANVIL_OP_SHL, 16, INT, U, SSE2, 1, "psllw"},
    {ANVIL_OP_SHL, 32, INT, U, SSE2, 1, "pslld"},
    {ANVIL_OP_SHL, 64, INT, U, SSE2, 1, "psllq"},
    {ANVIL_OP_SHR, 16, INT, U, SSE2, 1, "psrlw"},
    {ANVIL_OP_SHR, 32, INT, U, SSE2, 1, "psrld"},
    {ANVIL_OP_SHR, 64, INT, U, SSE2, 1, "psrlq"},
    {ANVIL_OP_SAR, 16, INT, U, SSE2, 1, "psraw"},
    {ANVIL_OP_SAR, 32, INT, U, SSE2, 1, "psrad"},

    /* Lane compares produce all-ones / zero masks.  The printer swaps
     * operands for < and <=, and complements (pxor with all ones) for !=,
     * >= and <=.  Unsigned order compares a lane with the unsigned max/min
     * of both: a <=u b is max(a, b) == b. */
    {ANVIL_OP_CMP_EQ, 8, INT, L, SSE2, 1, "pcmpeqb"},
    {ANVIL_OP_CMP_EQ, 16, INT, L, SSE2, 1, "pcmpeqw"},
    {ANVIL_OP_CMP_EQ, 32, INT, L, SSE2, 1, "pcmpeqd"},
    {ANVIL_OP_CMP_EQ, 64, INT, L, SSE41, 1, "pcmpeqq"},
    {ANVIL_OP_CMP_NE, 8, INT, L, SSE2, 2, "pcmpeqb"},
    {ANVIL_OP_CMP_NE, 16, INT, L, SSE2, 2, "pcmpeqw"},
    {ANVIL_OP_CMP_NE, 32, INT, L, SSE2, 2, "pcmpeqd"},
    {ANVIL_OP_CMP_NE, 64, INT, L, SSE41, 2, "pcmpeqq"},
    {ANVIL_OP_CMP_GT, 8, INT, L, SSE2, 1, "pcmpgtb"},
    {ANVIL_OP_CMP_GT, 16, INT, L, SSE2, 1, "pcmpgtw"},
    {ANVIL_OP_CMP_GT, 32, INT, L, SSE2, 1, "pcmpgtd"},
    {ANVIL_OP_CMP_GT, 64, INT, L, SSE42, 1, "pcmpgtq"},
    {ANVIL_OP_CMP_LT, 8, INT, L, SSE2, 1, "pcmpgtb"},
    {ANVIL_OP_CMP_LT, 16, INT, L, SSE2, 1, "pcmpgtw"},
    {ANVIL_OP_CMP_LT, 32, INT, L, SSE2, 1, "pcmpgtd"},
    {ANVIL_OP_CMP_LT, 64, INT, L, SSE42, 1, "pcmpgtq"},
    {ANVIL_OP_CMP_GE, 8, INT, L, SSE2, 2, "pcmpgtb"},
    {ANVIL_OP_CMP_GE, 16, INT, L, SSE2, 2, "pcmpgtw"},
    {ANVIL_OP_CMP_GE, 32, INT, L, SSE2, 2, "pcmpgtd"},
    {ANVIL_OP_CMP_GE, 64, INT, L, SSE42, 2, "pcmpgtq"},
    {ANVIL_OP_CMP_LE, 8, INT, L, SSE2, 2, "pcmpgtb"},
    {ANVIL_OP_CMP_LE, 16, INT, L, SSE2, 2, "pcmpgtw"},
    {ANVIL_OP_CMP_LE, 32, INT, L, SSE2, 2, "pcmpgtd"},
    {ANVIL_OP_CMP_LE, 64, INT, L, SSE42, 2, "pcmpgtq"},
    {ANVIL_OP_CMP_ULE, 8, INT, L, SSE2, 2, "pmaxub"},
    {ANVIL_OP_CMP_ULE, 16, INT, L, SSE41, 2, "pmaxuw"},
    {ANVIL_OP_CMP_ULE, 32, INT, L, SSE41, 2, "pmaxud"},
    {ANVIL_OP_CMP_UGE, 8, INT, L, SSE2, 2, "pminub"},
    {ANVIL_OP_CMP_UGE, 16, INT, L, SSE41, 2, "pminuw"},
    {ANVIL_OP_CMP_UGE, 32, INT, L, SSE41, 2, "pminud"},
    {ANVIL_OP_CMP_UGT, 8, INT, L, SSE2, 3, "pmaxub"},
    {ANVIL_OP_CMP_UGT, 16, INT, L, SSE41, 3, "pmaxuw"},
    {ANVIL_OP_CMP_UGT, 32, INT, L, SSE41, 3, "pmaxud"},
    {ANVIL_OP_CMP_ULT, 8, INT, L, SSE2, 3, "pminub"},
    {ANVIL_OP_CMP_ULT, 16, INT, L, SSE41, 3, "pminuw"},
    {ANVIL_OP_CMP_ULT, 32, INT, L, SSE41, 3, "pminud"},
    /* Every predicate: one cmpps/cmppd, or two for one/ueq. */
    {ANVIL_OP_FCMP, 32, FP, L, SSE, 2, "cmpps"},
    {ANVIL_OP_FCMP, 64, FP, L, SSE2, 2, "cmppd"},

    /* (mask & then) | (~mask & else). */
    {ANVIL_OP_SELECT, 0, ANY, L, SSE2, 3, "pand"},

    /* Reductions fold halves (64, 32, 16, 8 bits) with the lane operation
     * named here.  REDUCE_OR's cost is that of a lane mask
     * (pmovmskb), the form every vectorized exit test uses. */
    {ANVIL_OP_REDUCE_OR, 0, INT, L, SSE2, 2, "por"},
    {ANVIL_OP_REDUCE_ADD, 8, INT, L, SSE2, 6, "paddb"},
    {ANVIL_OP_REDUCE_ADD, 16, INT, L, SSE2, 5, "paddw"},
    {ANVIL_OP_REDUCE_ADD, 32, INT, L, SSE2, 4, "paddd"},
    {ANVIL_OP_REDUCE_ADD, 64, INT, L, SSE2, 3, "paddq"},
    {ANVIL_OP_REDUCE_AND, 8, INT, L, SSE2, 6, "pand"},
    {ANVIL_OP_REDUCE_AND, 16, INT, L, SSE2, 5, "pand"},
    {ANVIL_OP_REDUCE_AND, 32, INT, L, SSE2, 4, "pand"},
    {ANVIL_OP_REDUCE_AND, 64, INT, L, SSE2, 3, "pand"},
    {ANVIL_OP_REDUCE_XOR, 8, INT, L, SSE2, 6, "pxor"},
    {ANVIL_OP_REDUCE_XOR, 16, INT, L, SSE2, 5, "pxor"},
    {ANVIL_OP_REDUCE_XOR, 32, INT, L, SSE2, 4, "pxor"},
    {ANVIL_OP_REDUCE_XOR, 64, INT, L, SSE2, 3, "pxor"},
    {ANVIL_OP_REDUCE_SMAX, 8, INT, L, SSE41, 6, "pmaxsb"},
    {ANVIL_OP_REDUCE_SMAX, 16, INT, L, SSE2, 5, "pmaxsw"},
    {ANVIL_OP_REDUCE_SMAX, 32, INT, L, SSE41, 4, "pmaxsd"},
    {ANVIL_OP_REDUCE_SMIN, 8, INT, L, SSE41, 6, "pminsb"},
    {ANVIL_OP_REDUCE_SMIN, 16, INT, L, SSE2, 5, "pminsw"},
    {ANVIL_OP_REDUCE_SMIN, 32, INT, L, SSE41, 4, "pminsd"},
    {ANVIL_OP_REDUCE_UMAX, 8, INT, L, SSE2, 6, "pmaxub"},
    {ANVIL_OP_REDUCE_UMAX, 16, INT, L, SSE41, 5, "pmaxuw"},
    {ANVIL_OP_REDUCE_UMAX, 32, INT, L, SSE41, 4, "pmaxud"},
    {ANVIL_OP_REDUCE_UMIN, 8, INT, L, SSE2, 6, "pminub"},
    {ANVIL_OP_REDUCE_UMIN, 16, INT, L, SSE41, 5, "pminuw"},
    {ANVIL_OP_REDUCE_UMIN, 32, INT, L, SSE41, 4, "pminud"},
    {ANVIL_OP_REDUCE_FADD, 32, FP, L, SSE, 4, "addps"},
    {ANVIL_OP_REDUCE_FADD, 64, FP, L, SSE2, 3, "addpd"},

    /* Narrow lanes summed into wider accumulator lanes (lane width is the
     * accumulator's).  The printer forms the partial sums in a temporary,
     * then adds them: bytes through psadbw against zero (each half's sum in
     * its low dword), signed words through pmaddwd by ones, unsigned words
     * as the two halves of each dword, signed bytes through pmaddubsw by
     * ones and pmaddwd. */
    {ANVIL_OP_ACCUM_ZEXT, 32, INT, W4, SSE2, 2, "psadbw"},
    {ANVIL_OP_ACCUM_ZEXT, 64, INT, W8, SSE2, 2, "psadbw"},
    {ANVIL_OP_ACCUM_ZEXT, 32, INT, W2, SSE2, 5, "psrld"},
    {ANVIL_OP_ACCUM_SEXT, 32, INT, W2, SSE2, 3, "pmaddwd"},
    {ANVIL_OP_ACCUM_SEXT, 32, INT, W4, SSSE3, 5, "pmaddubsw"},
};

const size_t x64_vector_isa_count = sizeof(x64_vector_isa) / sizeof(x64_vector_isa[0]);

unsigned x64_vector_operation_cost(anvil_backend_t *be, anvil_op_t operation, anvil_type_t *type, anvil_vector_shape_t shape)
{
    return anvil_vector_isa_cost(x64_vector_isa, x64_vector_isa_count, 16, be->ctx, operation, type, shape);
}
