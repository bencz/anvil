/* i386 scalar operations the IR prices on single values, as data: the cost
 * hook, MIR legality and the printer all read this table.  The FP unit is
 * SSE2 when the CPU has it and x87 otherwise, so SSE2 rows come first and
 * the x87 rows (any width) serve every other CPU. */
#include "x86_32_internal.h"

#define SSE2 ANVIL_FEATURE_X86_SSE2
#define S ANVIL_VECTOR_SHAPE_SCALAR
#define FP ANVIL_VECTOR_LANES_FP
#define INT ANVIL_VECTOR_LANES_INT

const anvil_vector_isa_t x86_scalar_isa[] = {
    {ANVIL_OP_FSQRT, 32, FP, S, SSE2, 4, "sqrtss"},
    {ANVIL_OP_FSQRT, 64, FP, S, SSE2, 4, "sqrtsd"},
    {ANVIL_OP_FSQRT, 0, FP, S, 0, 4, "fsqrt"},
    /* High half of a product: one-operand mull/imull into edx:eax. */
    {ANVIL_OP_UMULH, 32, INT, S, 0, 3, "mull"},
    {ANVIL_OP_SMULH, 32, INT, S, 0, 3, "imull"},
};

const size_t x86_scalar_isa_count = sizeof(x86_scalar_isa) / sizeof(x86_scalar_isa[0]);

unsigned x86_scalar_operation_cost(anvil_backend_t *be, anvil_op_t operation, anvil_type_t *type, anvil_vector_shape_t shape)
{
    return anvil_vector_isa_cost(x86_scalar_isa, x86_scalar_isa_count, 0, be->ctx, operation, type, shape);
}
