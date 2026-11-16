/* SystemZ scalar operations as data: the cost hook, MIR legality and the
 * printer read this table.  RLL/RLLG (rotate left single logical) exist on
 * every z/Architecture processor, so the table serves the 64-bit variant
 * only. */
#include "systemz_internal.h"

#define INT ANVIL_VECTOR_LANES_INT
#define S ANVIL_VECTOR_SHAPE_SCALAR

static const anvil_vector_isa_t systemz_scalar_isa[] = {
    {ANVIL_OP_ROTL, 32, INT, S, 0, 1, "RLL"},
    {ANVIL_OP_ROTL, 64, INT, S, 0, 1, "RLLG"},
};

const anvil_vector_isa_t *systemz_scalar_table(const anvil_mainframe_target_desc_t *desc, size_t *count)
{
    *count = desc && desc->has_64bit_gprs ? sizeof(systemz_scalar_isa) / sizeof(systemz_scalar_isa[0]) : 0;
    return systemz_scalar_isa;
}

unsigned anvil_zarch_vector_operation_cost(anvil_backend_t *be, anvil_op_t operation, anvil_type_t *type, anvil_vector_shape_t shape)
{
    size_t count;
    const anvil_vector_isa_t *table = systemz_scalar_table(anvil_mainframe_get_target_desc(ANVIL_MAINFRAME_VARIANT_ZARCH), &count);
    return anvil_vector_isa_cost(table, count, 16, be->ctx, operation, type, shape);
}
