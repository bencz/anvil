#include "anvil/anvil_internal.h"

bool anvil_op_is_atomic(anvil_op_t op)
{
    return op >= ANVIL_OP_ATOMIC_LOAD && op <= ANVIL_OP_ATOMIC_FENCE;
}

bool anvil_atomic_type_valid(const anvil_type_t *type)
{
    return type && ((anvil_type_is_integer((anvil_type_t *)type) && type->kind != ANVIL_TYPE_I1) || type->kind == ANVIL_TYPE_PTR) &&
           (type->size == 1 || type->size == 2 || type->size == 4 || type->size == 8);
}

bool anvil_atomic_info_valid(anvil_op_t op, const anvil_atomic_info_t *info)
{
    if (!info || !anvil_op_is_atomic(op) || (unsigned)info->order > ANVIL_ORDER_SEQ_CST ||
        (unsigned)info->failure_order > ANVIL_ORDER_SEQ_CST || (unsigned)info->rmw > ANVIL_ATOMIC_XOR)
        return false;

    if (op == ANVIL_OP_ATOMIC_LOAD && (info->order == ANVIL_ORDER_RELEASE || info->order == ANVIL_ORDER_ACQ_REL))
        return false;

    if (op == ANVIL_OP_ATOMIC_STORE && (info->order == ANVIL_ORDER_ACQUIRE || info->order == ANVIL_ORDER_ACQ_REL))
        return false;

    if (op != ANVIL_OP_ATOMIC_RMW && info->rmw != ANVIL_ATOMIC_EXCHANGE)
        return false;

    if (op != ANVIL_OP_ATOMIC_CMPXCHG)
        return info->failure_order == ANVIL_ORDER_RELAXED;

    /* C11-compatible success/failure ordering. There is no write on failure. */
    switch (info->failure_order)
    {
        case ANVIL_ORDER_RELAXED:
            return true;
        case ANVIL_ORDER_ACQUIRE:
            return info->order == ANVIL_ORDER_ACQUIRE || info->order == ANVIL_ORDER_ACQ_REL || info->order == ANVIL_ORDER_SEQ_CST;
        case ANVIL_ORDER_SEQ_CST:
            return info->order == ANVIL_ORDER_SEQ_CST;
        default:
            return false;
    }
}

bool anvil_atomic_is_lock_free(anvil_ctx_t *ctx, anvil_type_t *type)
{
    return ctx && type && type->owner_ctx == ctx && anvil_atomic_type_valid(type) && ctx->backend &&
           ctx->backend->ops->atomic_is_lock_free && ctx->backend->ops->atomic_is_lock_free(ctx->backend, type);
}
