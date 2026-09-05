/*
 * ANVIL - IR Builder implementation
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>

static anvil_value_t *finish_value(anvil_ctx_t *ctx, anvil_instr_t *instr)
{
    if (!instr || !anvil_instr_insert(ctx, instr)) return NULL;
    return instr->result;
}

static bool add_operands(anvil_instr_t *instr,
                         anvil_value_t *const *values,
                         size_t count)
{
    return anvil_instr_add_operands(instr, values, count);
}

static void builder_invalid(anvil_ctx_t *ctx, const char *message)
{
    if (ctx) anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG, "%s", message);
}

static anvil_func_t *builder_current_func(anvil_ctx_t *ctx)
{
    return ctx && ctx->insert_block ? ctx->insert_block->parent : NULL;
}

static bool builder_value_is_usable(anvil_ctx_t *ctx,
                                    const anvil_value_t *value)
{
    if (!ctx || !value || !value->type || value->owner_ctx != ctx ||
        value->type->owner_ctx != ctx) return false;
    anvil_func_t *func = builder_current_func(ctx);
    if (!func) {
        return value->owner_module == NULL &&
               value->kind != ANVIL_VAL_PARAM &&
               value->kind != ANVIL_VAL_INSTR;
    }
    if (!func->parent || ctx->insert_block->owner_module != func->parent)
        return false;
    if (value->owner_module && value->owner_module != func->parent) return false;
    if (value->kind == ANVIL_VAL_PARAM)
        return value->data.param.func == func;
    if (value->kind == ANVIL_VAL_INSTR)
        return value->data.instr && value->data.instr->parent &&
               value->data.instr->parent->parent == func;
    return true;
}

static bool builder_target_is_current_func(anvil_ctx_t *ctx,
                                           const anvil_block_t *block)
{
    anvil_func_t *func = builder_current_func(ctx);
    if (!func || !block || block->owner_module != func->parent ||
        block->parent != func) return false;
    for (const anvil_block_t *it = func->blocks; it; it = it->next)
        if (it == block) return true;
    return false;
}

static bool next_capacity(size_t current, size_t needed,
                          size_t element_size, size_t *result)
{
    size_t capacity = current ? current : 4;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    if (element_size != 0 && capacity > SIZE_MAX / element_size) return false;
    *result = capacity;
    return true;
}

/* Helper to create binary operation */
static anvil_value_t *build_binop(anvil_ctx_t *ctx, anvil_op_t op,
                                   anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    if (!ctx || !builder_value_is_usable(ctx, lhs) ||
        !builder_value_is_usable(ctx, rhs)) {
        if (ctx) anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                                 "Binary builder requires two operands");
        return NULL;
    }
    
    bool sem_ok = (op == ANVIL_OP_SHL || op == ANVIL_OP_SHR ||
                   op == ANVIL_OP_SAR)
        ? (anvil_type_is_integer(lhs->type) &&
           lhs->type->kind != ANVIL_TYPE_I1 &&
           anvil_type_is_integer(rhs->type) &&
           rhs->type->kind != ANVIL_TYPE_I1)
        : anvil_sem_binary_types(op, lhs->type, rhs->type, lhs->type);
    if (!sem_ok) {
        builder_invalid(ctx, "Binary opcode/operand types are incompatible");
        return NULL;
    }
    anvil_instr_t *instr = anvil_instr_create(ctx, op, lhs->type, name);
    if (!instr) return NULL;
    
    anvil_value_t *operands[] = { lhs, rhs };
    if (!add_operands(instr, operands, 2)) return NULL;
    return finish_value(ctx, instr);
}

/* Helper to create unary operation */
static anvil_value_t *build_unop(anvil_ctx_t *ctx, anvil_op_t op,
                                  anvil_value_t *val, const char *name)
{
    if (!ctx || !builder_value_is_usable(ctx, val)) {
        if (ctx) anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                                 "Unary builder requires an operand");
        return NULL;
    }
    
    if (!anvil_sem_unary_types(op, val->type, val->type)) {
        builder_invalid(ctx, "Unary opcode/operand type is incompatible");
        return NULL;
    }
    anvil_instr_t *instr = anvil_instr_create(ctx, op, val->type, name);
    if (!instr) return NULL;
    
    if (!anvil_instr_add_operand(instr, val)) return NULL;
    return finish_value(ctx, instr);
}

/* Helper to create comparison operation */
static anvil_value_t *build_cmp(anvil_ctx_t *ctx, anvil_op_t op,
                                 anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    if (!ctx || !builder_value_is_usable(ctx, lhs) ||
        !builder_value_is_usable(ctx, rhs)) {
        builder_invalid(ctx, "Comparison builder requires two operands");
        return NULL;
    }

    if (!anvil_sem_cmp_types(op, lhs->type, rhs->type, ctx->type_i1)) {
        builder_invalid(ctx, "Comparison opcode/operand types are incompatible");
        return NULL;
    }
    /* Comparison result is the first-class i1 boolean type. */
    anvil_instr_t *instr = anvil_instr_create(ctx, op, ctx->type_i1, name);
    if (!instr) return NULL;

    anvil_value_t *operands[] = { lhs, rhs };
    if (!add_operands(instr, operands, 2)) return NULL;
    return finish_value(ctx, instr);
}

/* Helper to create a one-operand, result-typed instruction (trunc/zext/sext/
 * fp convert/bitcast/ptr-int cast). */
static anvil_value_t *build_cast(anvil_ctx_t *ctx, anvil_op_t op,
                                  anvil_value_t *val, anvil_type_t *type, const char *name)
{
    if (!ctx || !builder_value_is_usable(ctx, val) || !type ||
        type->owner_ctx != ctx) {
        builder_invalid(ctx, "Cast builder requires a value and result type");
        return NULL;
    }

    if (!anvil_sem_cast_types(op, val->type, type)) {
        builder_invalid(ctx, "Cast opcode/source/result types are incompatible");
        return NULL;
    }
    anvil_instr_t *instr = anvil_instr_create(ctx, op, type, name);
    if (!instr) return NULL;

    if (!anvil_instr_add_operand(instr, val)) return NULL;
    return finish_value(ctx, instr);
}

/* Arithmetic operations */
anvil_value_t *anvil_build_add(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_ADD, lhs, rhs, name);
}

anvil_value_t *anvil_build_sub(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_SUB, lhs, rhs, name);
}

anvil_value_t *anvil_build_mul(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_MUL, lhs, rhs, name);
}

anvil_value_t *anvil_build_sdiv(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_SDIV, lhs, rhs, name);
}

anvil_value_t *anvil_build_udiv(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_UDIV, lhs, rhs, name);
}

anvil_value_t *anvil_build_smod(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_SMOD, lhs, rhs, name);
}

anvil_value_t *anvil_build_umod(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_UMOD, lhs, rhs, name);
}

anvil_value_t *anvil_build_neg(anvil_ctx_t *ctx, anvil_value_t *val, const char *name)
{
    return build_unop(ctx, ANVIL_OP_NEG, val, name);
}

/* Bitwise operations */
anvil_value_t *anvil_build_and(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_AND, lhs, rhs, name);
}

anvil_value_t *anvil_build_or(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_OR, lhs, rhs, name);
}

anvil_value_t *anvil_build_xor(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_XOR, lhs, rhs, name);
}

anvil_value_t *anvil_build_not(anvil_ctx_t *ctx, anvil_value_t *val, const char *name)
{
    return build_unop(ctx, ANVIL_OP_NOT, val, name);
}

anvil_value_t *anvil_build_shl(anvil_ctx_t *ctx, anvil_value_t *val, anvil_value_t *amt, const char *name)
{
    return build_binop(ctx, ANVIL_OP_SHL, val, amt, name);
}

anvil_value_t *anvil_build_shr(anvil_ctx_t *ctx, anvil_value_t *val, anvil_value_t *amt, const char *name)
{
    return build_binop(ctx, ANVIL_OP_SHR, val, amt, name);
}

anvil_value_t *anvil_build_sar(anvil_ctx_t *ctx, anvil_value_t *val, anvil_value_t *amt, const char *name)
{
    return build_binop(ctx, ANVIL_OP_SAR, val, amt, name);
}

/* Comparison operations */
anvil_value_t *anvil_build_cmp_eq(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_EQ, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_ne(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_NE, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_lt(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_LT, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_le(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_LE, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_gt(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_GT, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_ge(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_GE, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_ult(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_ULT, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_ule(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_ULE, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_ugt(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_UGT, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_uge(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_UGE, lhs, rhs, name);
}

/* Memory operations */
anvil_value_t *anvil_build_va_start(anvil_ctx_t *ctx, const char *name)
{
    anvil_block_t *block = ctx ? ctx->insert_block : NULL;
    if (!block || !block->parent || !block->parent->type->data.func.variadic)
    {
        builder_invalid(ctx, "va_start requires a variadic function");
        return NULL;
    }

    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_VA_START, pointer, name);
    return instr ? finish_value(ctx, instr) : NULL;
}

anvil_value_t *anvil_build_va_copy(anvil_ctx_t *ctx, anvil_value_t *cursor, const char *name)
{
    if (!ctx || !builder_value_is_usable(ctx, cursor) || !ctx->backend || !ctx->backend->ops->build_va_copy)
    {
        builder_invalid(ctx, "va_copy requires a supported cursor ABI");
        return NULL;
    }

    return ctx->backend->ops->build_va_copy(ctx->backend, cursor, name);
}

bool anvil_build_va_copy_into(anvil_ctx_t *ctx, anvil_value_t *destination, anvil_value_t *cursor)
{
    if (!ctx || !builder_value_is_usable(ctx, destination) || !builder_value_is_usable(ctx, cursor) ||
        !anvil_type_is_pointer(destination->type) || !ctx->backend || !ctx->backend->ops->build_va_copy_into)
    {
        builder_invalid(ctx, "va_copy_into requires native cursor storage and a supported cursor ABI");
        return false;
    }

    return ctx->backend->ops->build_va_copy_into(ctx->backend, destination, cursor);
}

anvil_value_t *anvil_build_va_arg(anvil_ctx_t *ctx, anvil_value_t *cursor_storage, anvil_type_t *type, const char *name)
{
    if (!ctx || !type || type->owner_ctx != ctx || !anvil_sem_type_is_sized(type) ||
        !builder_value_is_usable(ctx, cursor_storage) || !ctx->backend || !ctx->backend->ops->build_va_arg)
    {
        builder_invalid(ctx, "va_arg requires a supported cursor ABI and a sized type");
        return NULL;
    }

    return ctx->backend->ops->build_va_arg(ctx->backend, cursor_storage, type, name);
}

anvil_value_t *anvil_build_alloca(anvil_ctx_t *ctx, anvil_type_t *type, const char *name)
{
    if (!ctx || !type || type->owner_ctx != ctx ||
        !anvil_sem_type_is_sized(type)) {
        builder_invalid(ctx, "Alloca builder requires an element type");
        return NULL;
    }

    anvil_type_t *ptr_type = anvil_type_ptr(ctx, type);
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_ALLOCA, ptr_type, name);
    if (!instr) return NULL;

    return finish_value(ctx, instr);
}

/* Dynamic alloca: stack-allocate `count` elements of `type`. The size is
 * computed at runtime as count * sizeof(type), rounded to the target's
 * stack alignment. Returns a pointer to the allocated region. Backends
 * distinguish this from the static form by checking num_operands > 0. */
anvil_value_t *anvil_build_alloca_dyn(anvil_ctx_t *ctx, anvil_type_t *type,
                                       anvil_value_t *count, const char *name)
{
    if (!ctx || !type || type->owner_ctx != ctx ||
        !anvil_sem_type_is_sized(type) ||
        !builder_value_is_usable(ctx, count) ||
        !anvil_type_is_integer(count->type)) {
        builder_invalid(ctx, "Dynamic alloca requires a type and count");
        return NULL;
    }

    anvil_type_t *ptr_type = anvil_type_ptr(ctx, type);
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_ALLOCA, ptr_type, name);
    if (!instr) return NULL;

    if (!anvil_instr_add_operand(instr, count)) return NULL;
    /* aux_type carries the element type so the backend knows element size. */
    instr->aux_type = type;
    return finish_value(ctx, instr);
}

static anvil_instr_t *build_atomic(anvil_ctx_t *ctx, anvil_op_t op, anvil_value_t **operands, size_t count,
                                   anvil_atomic_info_t info, const char *name)
{
    if (!ctx || !builder_current_func(ctx) || !anvil_atomic_info_valid(op, &info))
    {
        builder_invalid(ctx, "Invalid atomic operation or memory ordering");
        return NULL;
    }

    for (size_t index = 0; index < count; index++)
    {
        if (!builder_value_is_usable(ctx, operands[index]))
        {
            builder_invalid(ctx, "Atomic operand belongs to another function or context");
            return NULL;
        }
    }

    anvil_type_t *object = count ? anvil_sem_memory_object_type(operands[0]) : ctx->type_void;
    if (count && !anvil_atomic_type_valid(object))
    {
        builder_invalid(ctx, "Atomic storage requires a scalar integer or pointer type");
        return NULL;
    }

    for (size_t index = 1; index < count; index++)
    {
        if (!anvil_types_equal(object, operands[index]->type))
        {
            builder_invalid(ctx, "Atomic value and storage types must match");
            return NULL;
        }
    }

    if (op == ANVIL_OP_ATOMIC_RMW && info.rmw != ANVIL_ATOMIC_EXCHANGE && !anvil_type_is_integer(object))
    {
        builder_invalid(ctx, "Arithmetic atomic RMW requires an integer object");
        return NULL;
    }

    bool has_result = op != ANVIL_OP_ATOMIC_STORE && op != ANVIL_OP_ATOMIC_FENCE;
    anvil_instr_t *instr = anvil_instr_create(ctx, op, has_result ? object : ctx->type_void, name);
    if (!instr || !anvil_instr_add_operands(instr, operands, count))
        return NULL;

    instr->atomic = info;
    if (!anvil_instr_insert(ctx, instr))
        return NULL;

    return instr;
}

anvil_value_t *anvil_build_atomic_load(anvil_ctx_t *ctx, anvil_value_t *pointer, anvil_memory_order_t order, const char *name)
{
    anvil_atomic_info_t info = { .order = order };
    anvil_instr_t *instr = build_atomic(ctx, ANVIL_OP_ATOMIC_LOAD, &pointer, 1, info, name);
    return instr ? instr->result : NULL;
}

bool anvil_build_atomic_store(anvil_ctx_t *ctx, anvil_value_t *value, anvil_value_t *pointer, anvil_memory_order_t order)
{
    anvil_value_t *operands[] = { pointer, value };
    anvil_atomic_info_t info = { .order = order };
    return build_atomic(ctx, ANVIL_OP_ATOMIC_STORE, operands, 2, info, NULL) != NULL;
}

anvil_value_t *anvil_build_atomic_rmw(anvil_ctx_t *ctx, anvil_atomic_rmw_t operation, anvil_value_t *pointer, anvil_value_t *value,
                                    anvil_memory_order_t order, const char *name)
{
    anvil_value_t *operands[] = { pointer, value };
    anvil_atomic_info_t info = { .order = order, .rmw = operation };
    anvil_instr_t *instr = build_atomic(ctx, ANVIL_OP_ATOMIC_RMW, operands, 2, info, name);
    return instr ? instr->result : NULL;
}

anvil_value_t *anvil_build_atomic_cmpxchg(anvil_ctx_t *ctx, anvil_value_t *pointer, anvil_value_t *expected, anvil_value_t *desired,
                                        anvil_memory_order_t success, anvil_memory_order_t failure, const char *name)
{
    anvil_value_t *operands[] = { pointer, expected, desired };
    anvil_atomic_info_t info = { .order = success, .failure_order = failure };
    anvil_instr_t *instr = build_atomic(ctx, ANVIL_OP_ATOMIC_CMPXCHG, operands, 3, info, name);
    return instr ? instr->result : NULL;
}

bool anvil_build_atomic_fence(anvil_ctx_t *ctx, anvil_memory_order_t order)
{
    anvil_atomic_info_t info = { .order = order };
    return build_atomic(ctx, ANVIL_OP_ATOMIC_FENCE, NULL, 0, info, NULL) != NULL;
}

static bool valid_memory_access(anvil_ctx_t *ctx, const anvil_type_t *type, const anvil_memory_access_t *access)
{
    if (!access || access->alignment == 0)
        return true;

    size_t alignment = access->alignment;
    if ((alignment & (alignment - 1)) != 0 || alignment < type->align)
    {
        builder_invalid(ctx, "Memory alignment must be a power of two at least as large as the type alignment");
        return false;
    }

    return true;
}

anvil_value_t *anvil_build_load(anvil_ctx_t *ctx, anvil_type_t *type, anvil_value_t *ptr, const char *name)
{
    return anvil_build_load_ex(ctx, type, ptr, NULL, name);
}

anvil_value_t *anvil_build_load_ex(anvil_ctx_t *ctx, anvil_type_t *type, anvil_value_t *ptr,
                                  const anvil_memory_access_t *access, const char *name)
{
    if (!ctx || !type || type->owner_ctx != ctx ||
        !builder_value_is_usable(ctx, ptr) ||
        !anvil_types_equal(anvil_sem_memory_object_type(ptr), type)) {
        builder_invalid(ctx, "Load builder requires a type and pointer");
        return NULL;
    }
    
    if (!valid_memory_access(ctx, type, access))
        return NULL;

    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_LOAD, type, name);
    if (!instr) return NULL;

    if (access)
        instr->memory_access = *access;
    
    if (!anvil_instr_add_operand(instr, ptr)) return NULL;
    return finish_value(ctx, instr);
}

bool anvil_build_store(anvil_ctx_t *ctx, anvil_value_t *val, anvil_value_t *ptr)
{
    return anvil_build_store_ex(ctx, val, ptr, NULL);
}

bool anvil_build_store_ex(anvil_ctx_t *ctx, anvil_value_t *val, anvil_value_t *ptr, const anvil_memory_access_t *access)
{
    if (!ctx || !builder_value_is_usable(ctx, val) ||
        !builder_value_is_usable(ctx, ptr) ||
        !anvil_types_equal(anvil_sem_memory_object_type(ptr), val->type)) {
        builder_invalid(ctx, "Store builder requires a value and pointer");
        return false;
    }
    
    if (!valid_memory_access(ctx, val->type, access))
        return false;

    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_STORE, ctx->type_void, NULL);
    if (!instr) return false;

    if (access)
        instr->memory_access = *access;
    
    anvil_value_t *operands[] = { val, ptr };
    if (!add_operands(instr, operands, 2)) return false;
    return anvil_instr_insert(ctx, instr);
}

anvil_value_t *anvil_build_gep(anvil_ctx_t *ctx, anvil_type_t *type, anvil_value_t *ptr,
                                anvil_value_t **indices, size_t num_indices, const char *name)
{
    if (!ctx || !type || type->owner_ctx != ctx ||
        !builder_value_is_usable(ctx, ptr) ||
        !indices || num_indices == 0 ||
        num_indices == SIZE_MAX) {
        builder_invalid(ctx, "GEP requires source type, typed base, and at least one index");
        return NULL;
    }

    anvil_type_t *base_elem = NULL;
    if (ptr->type && ptr->type->kind == ANVIL_TYPE_PTR)
        base_elem = ptr->type->data.pointee;
    else if (ptr->kind == ANVIL_VAL_GLOBAL)
        base_elem = ptr->type;
    if (!base_elem || !anvil_types_equal(base_elem, type)) {
        builder_invalid(ctx, "GEP base does not point to its source element type");
        return NULL;
    }

    anvil_type_t *current = type;
    int64_t constant_offset = 0;
    for (size_t i = 0; i < num_indices; i++) {
        anvil_value_t *index = indices[i];
        anvil_gep_step_t step;
        if (!builder_value_is_usable(ctx, index) ||
            !anvil_gep_analyze_step(&current, index, i, &step)) {
            builder_invalid(ctx, "Invalid GEP index/type walk (struct indices must be constant)");
            return NULL;
        }
        if (index->kind == ANVIL_VAL_CONST_INT) {
            int64_t ignored;
            if (!anvil_gep_const_step_offset(&step, index, &ignored)) {
                builder_invalid(ctx, "Constant GEP byte offset overflows int64");
                return NULL;
            }
            if (!anvil_gep_accumulate_offset(&constant_offset, ignored)) {
                builder_invalid(ctx, "Accumulated constant GEP offset overflows int64");
                return NULL;
            }
        }
    }

    anvil_type_t *ptr_type = anvil_type_ptr(ctx, current);
    if (!ptr_type) return NULL;
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_GEP, ptr_type, name);
    if (!instr) return NULL;
    instr->aux_type = type;
    if (!anvil_instr_reserve_operands(instr, num_indices + 1) ||
        !anvil_instr_add_operand(instr, ptr) ||
        !add_operands(instr, indices, num_indices)) return NULL;
    return finish_value(ctx, instr);
}

anvil_value_t *anvil_build_struct_gep(anvil_ctx_t *ctx, anvil_type_t *struct_type,
                                       anvil_value_t *ptr, unsigned field_idx, const char *name)
{
    if (!ctx || !struct_type ||
        struct_type->owner_ctx != ctx ||
        !builder_value_is_usable(ctx, ptr) ||
        struct_type->kind != ANVIL_TYPE_STRUCT ||
        !struct_type->data.struc.complete || !ptr->type ||
        ptr->type->kind != ANVIL_TYPE_PTR ||
        !anvil_types_equal(ptr->type->data.pointee, struct_type) ||
        field_idx >= struct_type->data.struc.num_fields) {
        builder_invalid(ctx, "Struct GEP received invalid arguments");
        return NULL;
    }
    
    /* Get field type and create pointer to it */
    anvil_type_t *field_type = struct_type->data.struc.fields[field_idx];
    anvil_type_t *ptr_type = anvil_type_ptr(ctx, field_type);
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_STRUCT_GEP, ptr_type, name);
    if (!instr) return NULL;
    
    /* Store base pointer as operand */
    /* Store field index as constant operand */
    anvil_value_t *idx_val = anvil_const_i32(ctx, (int32_t)field_idx);
    if (!idx_val) return NULL;
    anvil_value_t *operands[] = { ptr, idx_val };
    if (!add_operands(instr, operands, 2)) return NULL;
    
    /* Store struct type reference for offset calculation */
    instr->aux_type = struct_type;
    
    return finish_value(ctx, instr);
}

/* Control flow */
bool anvil_build_br(anvil_ctx_t *ctx, anvil_block_t *dest)
{
    if (!ctx || !builder_target_is_current_func(ctx, dest)) {
        builder_invalid(ctx, "Branch builder requires a destination");
        return false;
    }
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_BR, ctx->type_void, NULL);
    if (!instr) return false;
    
    instr->true_block = dest;
    return anvil_instr_insert(ctx, instr);
}

bool anvil_build_br_cond(anvil_ctx_t *ctx, anvil_value_t *cond,
                         anvil_block_t *then_block, anvil_block_t *else_block)
{
    if (!ctx || !builder_value_is_usable(ctx, cond) ||
        !anvil_sem_bool_type(cond->type) ||
        !builder_target_is_current_func(ctx, then_block) ||
        !builder_target_is_current_func(ctx, else_block)) {
        builder_invalid(ctx, "Conditional branch requires condition and destinations");
        return false;
    }
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_BR_COND, ctx->type_void, NULL);
    if (!instr) return false;
    
    if (!anvil_instr_add_operand(instr, cond)) return false;
    instr->true_block = then_block;
    instr->false_block = else_block;
    return anvil_instr_insert(ctx, instr);
}

anvil_instr_t *anvil_build_switch(anvil_ctx_t *ctx, anvil_value_t *value,
                                  anvil_block_t *default_block)
{
    if (!ctx || !builder_value_is_usable(ctx, value) ||
        !anvil_type_is_integer(value->type) ||
        value->type->kind == ANVIL_TYPE_I1 ||
        !builder_target_is_current_func(ctx, default_block)) {
        builder_invalid(ctx, "Switch builder requires selector and default block");
        return NULL;
    }

    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_SWITCH,
                                              ctx->type_void, NULL);
    if (!instr) return NULL;

    if (!anvil_instr_add_operand(instr, value)) return NULL;
    instr->true_block = default_block;
    if (!anvil_instr_insert(ctx, instr)) return NULL;
    return instr;
}

bool anvil_switch_add_case(anvil_instr_t *switch_instr,
                           anvil_value_t *case_value,
                           anvil_block_t *dest)
{
    if (!switch_instr || switch_instr->op != ANVIL_OP_SWITCH ||
        !case_value || !dest || switch_instr->num_operands == 0 ||
        switch_instr->num_operands != switch_instr->num_switch_cases + 1) {
        if (switch_instr && switch_instr->owner_ctx) {
            builder_invalid(switch_instr->owner_ctx,
                            "Invalid switch case insertion");
        }
        return false;
    }

    anvil_value_t *selector = switch_instr->operands[0];
    anvil_ctx_t *owner = switch_instr->owner_ctx;
    if (!owner || !switch_instr->parent ||
        !builder_value_is_usable(owner, case_value) ||
        case_value->kind != ANVIL_VAL_CONST_INT ||
        !anvil_types_equal(case_value->type, selector->type) ||
        !builder_target_is_current_func(owner, dest)) {
        builder_invalid(owner, "Switch case must be a same-type integer constant and same-function target");
        return false;
    }
    for (size_t i = 0; i < switch_instr->num_switch_cases; i++) {
        if (switch_instr->operands[i + 1]->data.u == case_value->data.u) {
            builder_invalid(owner, "Switch case value is duplicated");
            return false;
        }
    }
    if (switch_instr->num_switch_cases == SIZE_MAX) {
        anvil_set_error(switch_instr->owner_ctx, ANVIL_ERR_NOMEM,
                        "Switch case count overflow");
        return false;
    }
    size_t new_num_cases = switch_instr->num_switch_cases + 1;
    size_t new_num_operands = switch_instr->num_operands + 1;
    anvil_block_t **new_blocks = NULL;
    anvil_value_t **new_operands = NULL;
    size_t blocks_capacity = switch_instr->switch_capacity;
    size_t operands_capacity = switch_instr->operands_capacity;

    if (new_num_cases > switch_instr->switch_capacity) {
        if (!next_capacity(switch_instr->switch_capacity, new_num_cases,
                           sizeof(*new_blocks), &blocks_capacity)) {
            anvil_set_error(switch_instr->owner_ctx, ANVIL_ERR_NOMEM,
                            "Switch case allocation overflow");
            return false;
        }
        new_blocks = anvil_ctx_malloc(
            switch_instr->owner_ctx, blocks_capacity * sizeof(*new_blocks));
        if (!new_blocks) return false;
        if (switch_instr->num_switch_cases > 0) {
            memcpy(new_blocks, switch_instr->switch_blocks,
                   switch_instr->num_switch_cases * sizeof(*new_blocks));
        }
    }

    if (new_num_operands > switch_instr->operands_capacity) {
        if (!next_capacity(switch_instr->operands_capacity, new_num_operands,
                           sizeof(*new_operands), &operands_capacity)) {
            free(new_blocks);
            anvil_set_error(switch_instr->owner_ctx, ANVIL_ERR_NOMEM,
                            "Switch operand allocation overflow");
            return false;
        }
        new_operands = anvil_ctx_malloc(
            switch_instr->owner_ctx,
            operands_capacity * sizeof(*new_operands));
        if (!new_operands) {
            free(new_blocks);
            return false;
        }
        memcpy(new_operands, switch_instr->operands,
               switch_instr->num_operands * sizeof(*new_operands));
    }

    if (new_blocks) {
        free(switch_instr->switch_blocks);
        switch_instr->switch_blocks = new_blocks;
        switch_instr->switch_capacity = blocks_capacity;
    }
    if (new_operands) {
        free(switch_instr->operands);
        switch_instr->operands = new_operands;
        switch_instr->operands_capacity = operands_capacity;
    }
    switch_instr->operands[switch_instr->num_operands] = case_value;
    switch_instr->num_operands = new_num_operands;
    switch_instr->switch_blocks[switch_instr->num_switch_cases] = dest;
    switch_instr->num_switch_cases = new_num_cases;
    return true;
}

bool anvil_build_call_checked(anvil_ctx_t *ctx, anvil_value_t *callee,
                               anvil_value_t **args, size_t num_args,
                               const char *name, anvil_value_t **result)
{
    if (result) *result = NULL;
    if (!ctx || !builder_value_is_usable(ctx, callee)) {
        builder_invalid(ctx, "Call builder requires a callee");
        return false;
    }
    
    if ((num_args > 0 && !args) || num_args == SIZE_MAX) {
        builder_invalid(ctx, "Call builder received invalid arguments");
        return false;
    }
    anvil_type_t *fn_type = anvil_sem_callee_func_type(callee);
    anvil_cc_t effective_cc = ANVIL_CC_DEFAULT;
    if (!fn_type || fn_type->owner_ctx != ctx ||
        !fn_type->data.func.ret ||
        fn_type->data.func.ret->owner_ctx != ctx ||
        fn_type->data.func.ret->kind == ANVIL_TYPE_FUNC ||
        (fn_type->data.func.ret->kind != ANVIL_TYPE_VOID &&
         !anvil_sem_type_is_sized(fn_type->data.func.ret)) ||
        (fn_type->data.func.num_params > 0 &&
         !fn_type->data.func.params) ||
        !anvil_cc_resolve(ctx, fn_type->data.func.cc,
                                     &effective_cc) ||
        effective_cc != fn_type->data.func.cc ||
        (!fn_type->data.func.variadic &&
         num_args != fn_type->data.func.num_params) ||
        (fn_type->data.func.variadic &&
         num_args < fn_type->data.func.num_params)) {
        builder_invalid(ctx, "Call signature, result type, or argument count is incompatible");
        return false;
    }
    for (size_t i = 0; i < num_args; i++) {
        if (!builder_value_is_usable(ctx, args[i]) ||
            (i < fn_type->data.func.num_params &&
             (!fn_type->data.func.params[i] ||
              fn_type->data.func.params[i]->owner_ctx != ctx ||
              !anvil_sem_type_is_sized(fn_type->data.func.params[i]) ||
              !anvil_types_equal(args[i]->type,
                                 fn_type->data.func.params[i])))) {
            builder_invalid(ctx, "Call argument type/context does not match the signature");
            return false;
        }
    }
    anvil_type_t *ret_type = fn_type->data.func.ret;
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_CALL, ret_type, name);
    if (!instr) return false;
    instr->call_cc = effective_cc;
    if (!anvil_instr_reserve_operands(instr, num_args + 1) ||
        !anvil_instr_add_operand(instr, callee) ||
        !add_operands(instr, args, num_args) ||
        !anvil_instr_insert(ctx, instr)) {
        anvil_instr_discard_new(ctx, instr);
        return false;
    }
    if (result) *result = instr->result;
    return true;
}

bool anvil_build_ret(anvil_ctx_t *ctx, anvil_value_t *val)
{
    anvil_func_t *func = builder_current_func(ctx);
    if (!ctx || !func || !func->type || func->type->kind != ANVIL_TYPE_FUNC) {
        builder_invalid(ctx, "Return requires a live insertion function");
        return false;
    }
    anvil_type_t *expected = func->type->data.func.ret;
    if ((expected->kind == ANVIL_TYPE_VOID && val) ||
        (expected->kind != ANVIL_TYPE_VOID &&
         (!builder_value_is_usable(ctx, val) ||
          !anvil_types_equal(val->type, expected)))) {
        builder_invalid(ctx, "Return value does not match the function result type");
        return false;
    }
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_RET, ctx->type_void, NULL);
    if (!instr) return false;
    
    if (val) {
        if (!anvil_instr_add_operand(instr, val)) return false;
    }
    return anvil_instr_insert(ctx, instr);
}

bool anvil_build_ret_void(anvil_ctx_t *ctx)
{
    return anvil_build_ret(ctx, NULL);
}

/* Type conversions — all reduce to build_cast(op, val, type, name). */
anvil_value_t *anvil_build_trunc(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    return build_cast(ctx, ANVIL_OP_TRUNC, val, type, name);
}

anvil_value_t *anvil_build_zext(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    return build_cast(ctx, ANVIL_OP_ZEXT, val, type, name);
}

anvil_value_t *anvil_build_sext(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    return build_cast(ctx, ANVIL_OP_SEXT, val, type, name);
}

anvil_value_t *anvil_build_bitcast(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    return build_cast(ctx, ANVIL_OP_BITCAST, val, type, name);
}

anvil_value_t *anvil_build_ptrtoint(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    return build_cast(ctx, ANVIL_OP_PTRTOINT, val, type, name);
}

anvil_value_t *anvil_build_inttoptr(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    return build_cast(ctx, ANVIL_OP_INTTOPTR, val, type, name);
}

anvil_value_t *anvil_build_fptrunc(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    return build_cast(ctx, ANVIL_OP_FPTRUNC, val, type, name);
}

anvil_value_t *anvil_build_fpext(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    return build_cast(ctx, ANVIL_OP_FPEXT, val, type, name);
}

anvil_value_t *anvil_build_fptosi(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    return build_cast(ctx, ANVIL_OP_FPTOSI, val, type, name);
}

anvil_value_t *anvil_build_fptoui(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    return build_cast(ctx, ANVIL_OP_FPTOUI, val, type, name);
}

anvil_value_t *anvil_build_sitofp(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    return build_cast(ctx, ANVIL_OP_SITOFP, val, type, name);
}

anvil_value_t *anvil_build_uitofp(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    return build_cast(ctx, ANVIL_OP_UITOFP, val, type, name);
}

/* Floating-point operations */
anvil_value_t *anvil_build_fadd(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_FADD, lhs, rhs, name);
}

anvil_value_t *anvil_build_fsub(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_FSUB, lhs, rhs, name);
}

anvil_value_t *anvil_build_fmul(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_FMUL, lhs, rhs, name);
}

anvil_value_t *anvil_build_fdiv(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_FDIV, lhs, rhs, name);
}

anvil_value_t *anvil_build_fneg(anvil_ctx_t *ctx, anvil_value_t *val, const char *name)
{
    return build_unop(ctx, ANVIL_OP_FNEG, val, name);
}

anvil_value_t *anvil_build_fabs(anvil_ctx_t *ctx, anvil_value_t *val, const char *name)
{
    return build_unop(ctx, ANVIL_OP_FABS, val, name);
}

anvil_value_t *anvil_build_fcmp(anvil_ctx_t *ctx,
                                 anvil_fcmp_pred_t predicate,
                                 anvil_value_t *lhs, anvil_value_t *rhs,
                                 const char *name)
{
    if (!ctx) return NULL;
    if ((unsigned)predicate > (unsigned)ANVIL_FCMP_TRUE) {
        builder_invalid(ctx, "Invalid floating-point comparison predicate");
        return NULL;
    }
    anvil_value_t *result = build_cmp(ctx, ANVIL_OP_FCMP, lhs, rhs, name);
    if (result) result->data.instr->fcmp_pred = predicate;
    return result;
}

/* Misc */
anvil_value_t *anvil_build_phi(anvil_ctx_t *ctx, anvil_type_t *type, const char *name)
{
    if (!ctx || !type || type->owner_ctx != ctx ||
        !anvil_sem_type_is_sized(type)) {
        builder_invalid(ctx, "PHI builder requires a result type");
        return NULL;
    }
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_PHI, type, name);
    if (!instr) return NULL;
    
    return finish_value(ctx, instr);
}

bool anvil_phi_add_incoming(anvil_value_t *phi, anvil_value_t *val, anvil_block_t *block)
{
    if (!phi || !val || !block) {
        if (phi) builder_invalid(phi->owner_ctx, "Invalid PHI incoming");
        return false;
    }
    if (phi->kind != ANVIL_VAL_INSTR) {
        builder_invalid(phi->owner_ctx, "Value is not a PHI instruction");
        return false;
    }
    
    anvil_instr_t *instr = phi->data.instr;
    if (!instr || instr->op != ANVIL_OP_PHI || !instr->owner_ctx ||
        !instr->parent || !instr->parent->parent ||
        instr->num_operands != instr->num_phi_incoming) return false;
    anvil_func_t *func = instr->parent->parent;
    if (val->owner_ctx != instr->owner_ctx ||
        val->type->owner_ctx != instr->owner_ctx ||
        !anvil_types_equal(val->type, phi->type) ||
        (val->owner_module && val->owner_module != func->parent) ||
        (val->kind == ANVIL_VAL_PARAM && val->data.param.func != func) ||
        (val->kind == ANVIL_VAL_INSTR &&
         (!val->data.instr || !val->data.instr->parent ||
          val->data.instr->parent->parent != func)) ||
        block->owner_module != func->parent || block->parent != func) {
        builder_invalid(instr->owner_ctx,
                        "PHI incoming value/type/block belongs to another function or context");
        return false;
    }
    bool live_block = false;
    for (anvil_block_t *it = func->blocks; it; it = it->next)
        if (it == block) { live_block = true; break; }
    if (!live_block) {
        builder_invalid(instr->owner_ctx, "PHI incoming block is not live");
        return false;
    }
    for (size_t i = 0; i < instr->num_phi_incoming; i++) {
        if (instr->phi_blocks[i] == block) {
            builder_invalid(instr->owner_ctx,
                            "PHI already has an incoming value for this predecessor");
            return false;
        }
    }
    if (instr->num_phi_incoming == SIZE_MAX) {
        anvil_set_error(instr->owner_ctx, ANVIL_ERR_NOMEM,
                        "PHI incoming count overflow");
        return false;
    }
    size_t new_count = instr->num_phi_incoming + 1;
    anvil_block_t **new_blocks = NULL;
    anvil_value_t **new_operands = NULL;
    size_t blocks_capacity = instr->phi_capacity;
    size_t operands_capacity = instr->operands_capacity;

    if (new_count > instr->phi_capacity) {
        if (!next_capacity(instr->phi_capacity, new_count,
                           sizeof(*new_blocks), &blocks_capacity)) {
            anvil_set_error(instr->owner_ctx, ANVIL_ERR_NOMEM,
                            "PHI incoming allocation overflow");
            return false;
        }
        new_blocks = anvil_ctx_malloc(
            instr->owner_ctx, blocks_capacity * sizeof(*new_blocks));
        if (!new_blocks) return false;
        if (instr->num_phi_incoming > 0) {
            memcpy(new_blocks, instr->phi_blocks,
                   instr->num_phi_incoming * sizeof(*new_blocks));
        }
    }

    if (new_count > instr->operands_capacity) {
        if (!next_capacity(instr->operands_capacity, new_count,
                           sizeof(*new_operands), &operands_capacity)) {
            free(new_blocks);
            anvil_set_error(instr->owner_ctx, ANVIL_ERR_NOMEM,
                            "PHI operand allocation overflow");
            return false;
        }
        new_operands = anvil_ctx_malloc(
            instr->owner_ctx, operands_capacity * sizeof(*new_operands));
        if (!new_operands) {
            free(new_blocks);
            return false;
        }
        if (instr->num_operands > 0) {
            memcpy(new_operands, instr->operands,
                   instr->num_operands * sizeof(*new_operands));
        }
    }

    if (new_blocks) {
        free(instr->phi_blocks);
        instr->phi_blocks = new_blocks;
        instr->phi_capacity = blocks_capacity;
    }
    if (new_operands) {
        free(instr->operands);
        instr->operands = new_operands;
        instr->operands_capacity = operands_capacity;
    }
    instr->operands[instr->num_operands] = val;
    instr->num_operands = new_count;
    instr->phi_blocks[instr->num_phi_incoming] = block;
    instr->num_phi_incoming = new_count;
    return true;
}

anvil_value_t *anvil_build_select(anvil_ctx_t *ctx, anvil_value_t *cond,
                                   anvil_value_t *then_val, anvil_value_t *else_val, const char *name)
{
    if (!ctx || !builder_value_is_usable(ctx, cond) ||
        !builder_value_is_usable(ctx, then_val) ||
        !builder_value_is_usable(ctx, else_val) ||
        !anvil_sem_bool_type(cond->type) ||
        !anvil_types_equal(then_val->type, else_val->type)) {
        builder_invalid(ctx, "Select builder requires condition and alternatives");
        return NULL;
    }
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_SELECT, then_val->type, name);
    if (!instr) return NULL;
    
    anvil_value_t *operands[] = { cond, then_val, else_val };
    if (!add_operands(instr, operands, 3)) return NULL;
    return finish_value(ctx, instr);
}
