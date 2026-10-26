/*
 * MCC - Micro C Compiler
 * Code Generator - Variable-Length Arrays
 *
 * An object whose array type has a runtime dimension at any depth is one
 * contiguous run of its innermost elements.  Its lvalue is a pointer to the
 * innermost element type; subscripting scales the index by the runtime
 * element count of the subarray.  Each dimension length is evaluated once,
 * where the declarator is reached (C11 6.7.6.2p5), and reused afterwards.
 */

#include "codegen_internal.h"

static mcc_type_t *unwrap(mcc_type_t *type)
{
    while (type && type->kind == TYPE_TYPEDEF)
        type = type->data.typedef_ref.underlying;
    return type;
}

bool codegen_type_has_vla(mcc_type_t *type)
{
    for (type = unwrap(type); type && type->kind == TYPE_ARRAY;
         type = unwrap(type->data.array.element))
        if (type->data.array.is_vla) return true;
    return false;
}

mcc_type_t *codegen_vla_innermost(mcc_type_t *type)
{
    type = unwrap(type);
    while (type && type->kind == TYPE_ARRAY)
        type = unwrap(type->data.array.element);
    return type;
}

/* Element counts and offsets are signed and pointer-sized (ptrdiff_t):
 * GEP indices are signed, so a backward step must stay negative. */
static anvil_type_t *index_type(mcc_codegen_t *cg)
{
    return codegen_type(cg, mcc_type_ptrdiff_t(cg->types));
}

/* Converts an integer value to ptrdiff_t from its IR type, so it does not
 * depend on the C type recorded for the expression. */
static anvil_value_t *to_index(mcc_codegen_t *cg, anvil_value_t *value,
                               const char *name)
{
    anvil_type_t *integer = index_type(cg);
    anvil_type_t *source = value ? anvil_value_get_type(value) : NULL;
    if (!source || !anvil_type_is_integer(source)) return NULL;
    unsigned from = anvil_type_bit_width(source);
    unsigned to = anvil_type_bit_width(integer);
    if (from < to)
        return anvil_type_is_signed(source)
            ? anvil_build_sext(cg->anvil_ctx, value, integer, name)
            : anvil_build_zext(cg->anvil_ctx, value, integer, name);
    if (from > to)
        return anvil_build_trunc(cg->anvil_ctx, value, integer, name);
    if (anvil_type_is_signed(source) != anvil_type_is_signed(integer))
        return anvil_build_bitcast(cg->anvil_ctx, value, integer, name);
    return value;
}

static anvil_value_t *find_length(mcc_codegen_t *cg, mcc_type_t *type)
{
    for (size_t i = cg->num_vla_lengths; i-- > 0;)
        if (cg->vla_lengths[i].type == type) return cg->vla_lengths[i].length;
    return NULL;
}

bool codegen_vla_bind(mcc_codegen_t *cg, mcc_type_t *type)
{
    type = unwrap(type);
    while (type && type->kind == TYPE_POINTER)        /* adjusted parameters */
        type = unwrap(type->data.pointer.pointee);
    for (; type && type->kind == TYPE_ARRAY;
         type = unwrap(type->data.array.element)) {
        if (!type->data.array.is_vla) continue;
        mcc_ast_node_t *expr = type->data.array.length_expr;
        if (!expr) {
            mcc_error(cg->mcc_ctx,
                      "variable-length array dimension has no length");
            return false;
        }
        anvil_value_t *length = to_index(cg, codegen_expr(cg, expr),
                                        "vla.length");
        if (!length) {
            mcc_error_at(cg->mcc_ctx, expr->location,
                         "cannot evaluate variable-length array dimension");
            return false;
        }
        if (cg->num_vla_lengths == cg->cap_vla_lengths) {
            size_t capacity = cg->cap_vla_lengths ? cg->cap_vla_lengths * 2 : 8;
            void *grown = mcc_realloc_array(cg->mcc_ctx, cg->vla_lengths,
                cg->cap_vla_lengths, capacity, sizeof(cg->vla_lengths[0]));
            if (!grown) return false;
            cg->vla_lengths = grown;
            cg->cap_vla_lengths = capacity;
        }
        cg->vla_lengths[cg->num_vla_lengths].type = type;
        cg->vla_lengths[cg->num_vla_lengths].length = length;
        cg->num_vla_lengths++;
    }
    return true;
}

anvil_value_t *codegen_vla_element_count(mcc_codegen_t *cg, mcc_type_t *type)
{
    anvil_type_t *integer = index_type(cg);
    type = unwrap(type);
    if (!type || type->kind != TYPE_ARRAY)
        return codegen_const_int_for_type(cg, integer, 1);

    anvil_value_t *inner = codegen_vla_element_count(cg, type->data.array.element);
    anvil_value_t *length = type->data.array.is_vla
        ? find_length(cg, type)
        : codegen_const_int_for_type(cg, integer,
                                     (int64_t)type->data.array.length);
    if (!length) {
        mcc_error(cg->mcc_ctx,
                  "variable-length array dimension used before its declaration");
        return NULL;
    }
    if (!inner) return NULL;
    return anvil_build_mul(cg->anvil_ctx, length, inner, "vla.count");
}

anvil_value_t *codegen_vla_decay(mcc_codegen_t *cg, mcc_type_t *array,
                                 anvil_value_t *object)
{
    mcc_type_t *element = unwrap(array)->data.array.element;
    return anvil_build_bitcast(cg->anvil_ctx, object,
        anvil_type_ptr(cg->anvil_ctx, codegen_type(cg, element)),
        "vla.decay");
}

anvil_value_t *codegen_vla_subscript(mcc_codegen_t *cg, mcc_type_t *element,
                                     anvil_value_t *base, mcc_ast_node_t *index)
{
    anvil_type_t *innermost = codegen_type(cg, codegen_vla_innermost(element));
    anvil_value_t *position = to_index(cg, codegen_expr(cg, index), "vla.index");
    anvil_value_t *count = codegen_vla_element_count(cg, element);
    if (!innermost || !position || !count || !base) return NULL;

    base = anvil_build_bitcast(cg->anvil_ctx, base,
                               anvil_type_ptr(cg->anvil_ctx, innermost),
                               "vla.base");
    anvil_value_t *offset = anvil_build_mul(cg->anvil_ctx, position, count,
                                            "vla.offset");
    anvil_value_t *address = anvil_build_gep(cg->anvil_ctx, innermost, base,
                                             &offset, 1, "vla.elem");
    /* A subarray that still has a runtime dimension stays an innermost
     * pointer; any other element gets its ordinary object pointer. */
    if (codegen_type_has_vla(element)) return address;
    return anvil_build_bitcast(cg->anvil_ctx, address,
        anvil_type_ptr(cg->anvil_ctx, codegen_type(cg, element)), "vla.object");
}

bool codegen_type_is_variably_modified(mcc_type_t *type)
{
    type = unwrap(type);
    while (type && type->kind == TYPE_POINTER)
        type = unwrap(type->data.pointer.pointee);
    return codegen_type_has_vla(type);
}

anvil_value_t *codegen_pointer_offset(mcc_codegen_t *cg, mcc_type_t *pointer_type,
                                      anvil_value_t *pointer, anvil_value_t *delta,
                                      bool subtract)
{
    mcc_type_t *pointee = unwrap(unwrap(pointer_type)->data.pointer.pointee);
    if (!pointer || !delta) return NULL;

    if (!codegen_type_has_vla(pointee)) {
        anvil_value_t *index = subtract
            ? anvil_build_neg(cg->anvil_ctx, delta, "ptr.negidx") : delta;
        /* Arithmetic on void * steps by bytes (GNU C). */
        anvil_type_t *element = pointee && pointee->kind != TYPE_VOID
            ? codegen_type(cg, pointee) : anvil_type_u8(cg->anvil_ctx);
        return anvil_build_gep(cg->anvil_ctx, element, pointer, &index, 1,
                               "ptr.gep");
    }

    /* Pointer to a runtime-sized object: step whole runtime elements. */
    anvil_type_t *innermost = codegen_type(cg, codegen_vla_innermost(pointee));
    anvil_value_t *count = codegen_vla_element_count(cg, pointee);
    anvil_value_t *steps = to_index(cg, delta, "ptr.steps");
    if (!innermost || !count || !steps) return NULL;
    anvil_value_t *offset = anvil_build_mul(cg->anvil_ctx, steps, count,
                                            "ptr.units");
    if (subtract)
        offset = anvil_build_neg(cg->anvil_ctx, offset, "ptr.negunits");
    anvil_value_t *base = anvil_build_bitcast(
        cg->anvil_ctx, pointer, anvil_type_ptr(cg->anvil_ctx, innermost),
        "ptr.units.base");
    anvil_value_t *moved = anvil_build_gep(cg->anvil_ctx, innermost, base,
                                           &offset, 1, "ptr.vla.gep");
    return anvil_build_bitcast(cg->anvil_ctx, moved,
                               anvil_value_get_type(pointer), "ptr.vla");
}

anvil_value_t *codegen_pointer_diff(mcc_codegen_t *cg, mcc_type_t *pointer_type,
                                    anvil_value_t *lhs, anvil_value_t *rhs)
{
    mcc_type_t *pointee = unwrap(unwrap(pointer_type)->data.pointer.pointee);
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(cg->anvil_ctx);
    anvil_type_t *integer = arch && arch->ptr_size == 4
        ? anvil_type_i32(cg->anvil_ctx) : anvil_type_i64(cg->anvil_ctx);
    if (!lhs || !rhs) return NULL;

    anvil_value_t *bytes = anvil_build_sub(
        cg->anvil_ctx,
        anvil_build_ptrtoint(cg->anvil_ctx, lhs, integer, "ptr.lhs"),
        anvil_build_ptrtoint(cg->anvil_ctx, rhs, integer, "ptr.rhs"),
        "ptr.diff");
    anvil_value_t *size;
    if (codegen_type_has_vla(pointee)) {
        anvil_value_t *count = codegen_vla_element_count(cg, pointee);
        if (!count) return NULL;
        size_t unit = mcc_type_sizeof(codegen_vla_innermost(pointee));
        anvil_value_t *units = anvil_build_mul(
            cg->anvil_ctx, count,
            codegen_const_int_for_type(cg, anvil_value_get_type(count),
                                       (int64_t)unit),
            "ptr.vla.size");
        size = to_index(cg, units, "ptr.size");
    } else {
        size_t unit = pointee && pointee->kind != TYPE_VOID
            ? mcc_type_sizeof(pointee) : 1;
        if (unit <= 1) return bytes;
        size = codegen_const_int_for_type(cg, integer, (int64_t)unit);
    }
    return anvil_build_sdiv(cg->anvil_ctx, bytes, size, "ptr.diff.elem");
}
