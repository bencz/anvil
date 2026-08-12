/*
 * MCC - Micro C Compiler
 * Code Generator - Type Conversion
 * 
 * This file handles conversion from MCC types to ANVIL types.
 */

#include "codegen_internal.h"

static mcc_type_t *codegen_type_unwrap(mcc_type_t *type)
{
    while (type && type->kind == TYPE_TYPEDEF) {
        type = type->data.typedef_ref.underlying;
    }
    return type;
}

static anvil_type_t *codegen_check_object_layout(mcc_codegen_t *cg,
                                                  mcc_type_t *source,
                                                  anvil_type_t *lowered)
{
    if (!lowered) return NULL;
    if (source && anvil_type_size(lowered) == source->size &&
        anvil_type_align(lowered) == source->align) return lowered;
    mcc_error(cg->mcc_ctx,
              "C/ANVIL DataLayout mismatch lowering '%s' (%zu/%zu vs %zu/%zu)",
              source ? mcc_type_kind_name(source->kind) : "missing",
              source ? source->size : 0, source ? source->align : 0,
              anvil_type_size(lowered), anvil_type_align(lowered));
    return NULL;
}

bool codegen_type_pass_by_reference(mcc_type_t *type)
{
    type = codegen_type_unwrap(type);
    return type && (type->kind == TYPE_STRUCT || type->kind == TYPE_UNION);
}

anvil_type_t *codegen_param_type(mcc_codegen_t *cg, mcc_type_t *type)
{
    anvil_type_t *value_type = codegen_type(cg, type);
    if (!value_type) return NULL;
    if (codegen_type_pass_by_reference(type)) {
        return anvil_type_ptr(cg->anvil_ctx, value_type);
    }
    return value_type;
}

static anvil_type_t *codegen_record_fail(mcc_codegen_t *cg,
                                         mcc_type_t *type,
                                         const char *message)
{
    type->anvil_lowering = false;
    type->anvil_lower_failed = true;
    if (message) mcc_error(cg->mcc_ctx, "%s", message);
    return NULL;
}

static anvil_type_t *codegen_record_type(mcc_codegen_t *cg, mcc_type_t *type)
{
    if (type->anvil_lower_failed) {
        mcc_error(cg->mcc_ctx, "record type lowering previously failed");
        return NULL;
    }

    anvil_type_t *record_type = type->anvil_cached;
    if (!record_type) {
        char record_name[64];
        snprintf(record_name, sizeof(record_name), "mcc.%s.%p",
                 type->kind == TYPE_UNION ? "union" : "struct",
                 (void *)type);
        record_type = anvil_type_named_struct(cg->anvil_ctx, record_name);
        if (!record_type) {
            return codegen_record_fail(cg, type,
                                       "failed to create ANVIL record type");
        }
        type->anvil_cached = record_type;
    }

    /* An incomplete tag intentionally lowers to an opaque identified struct.
     * If that same C tag is completed later, revisit it and install the body.
     * During recursive lowering the cached opaque type breaks the cycle. */
    if (!type->data.record.is_complete || type->anvil_body_lowered ||
        type->anvil_lowering) {
        return record_type;
    }
    type->anvil_lowering = true;

    size_t num_fields = 0;
    for (mcc_struct_field_t *field = type->data.record.fields; field;
         field = field->next) {
        if (field->bitfield_width != 0) {
            return codegen_record_fail(cg, type,
                                       "bit-field lowering is not implemented");
        }
        if (field->name || (field->type && mcc_type_is_record(field->type))) {
            if (num_fields == SIZE_MAX) {
                return codegen_record_fail(cg, type,
                                           "record field count overflow");
            }
            num_fields++;
        }
    }

    size_t field_capacity = num_fields;
    if (type->kind == TYPE_UNION && num_fields != 0) {
        if (field_capacity == SIZE_MAX) {
            return codegen_record_fail(cg, type,
                                       "union storage field count overflow");
        }
        field_capacity++;
    }
    anvil_type_t **field_types = field_capacity
        ? mcc_alloc_array(cg->mcc_ctx, field_capacity, sizeof(*field_types))
        : NULL;
    if (field_capacity && !field_types) {
        return codegen_record_fail(cg, type, NULL);
    }

    size_t index = 0;
    for (mcc_struct_field_t *field = type->data.record.fields; field;
         field = field->next) {
        if (field->name || (field->type && mcc_type_is_record(field->type))) {
            field_types[index] = codegen_type(cg, field->type);
            if (!field_types[index]) {
                return codegen_record_fail(cg, type, NULL);
            }
            index++;
        }
    }
    if (index != num_fields) {
        return codegen_record_fail(cg, type,
                                   "record field count changed during lowering");
    }

    size_t body_fields = num_fields;
    if (type->kind == TYPE_UNION && num_fields != 0) {
        anvil_type_t *anchor = field_types[0];
        size_t max_size = anvil_type_size(anchor);
        size_t max_align = anvil_type_align(anchor);
        for (size_t field = 1; field < num_fields; field++) {
            size_t field_size = anvil_type_size(field_types[field]);
            size_t field_align = anvil_type_align(field_types[field]);
            if (field_size > max_size) max_size = field_size;
            if (field_align > max_align) {
                max_align = field_align;
                anchor = field_types[field];
            }
        }
        field_types[0] = anchor;
        body_fields = 1;
        size_t anchor_size = anvil_type_size(anchor);
        if (anchor_size < max_size) {
            field_types[1] = anvil_type_array(cg->anvil_ctx,
                anvil_type_u8(cg->anvil_ctx), max_size - anchor_size);
            if (!field_types[1]) {
                return codegen_record_fail(cg, type, NULL);
            }
            body_fields = 2;
        }
    }

    if (!anvil_type_struct_set_body(record_type, field_types, body_fields,
                                    false)) {
        mcc_error(cg->mcc_ctx, "failed to lower %s layout: %s",
                  type->kind == TYPE_UNION ? "union" : "struct",
                  anvil_ctx_get_error(cg->anvil_ctx));
        return codegen_record_fail(cg, type, NULL);
    }
    type->anvil_lowering = false;
    type->anvil_body_lowered = true;
    if (!codegen_check_object_layout(cg, type, record_type)) {
        type->anvil_lower_failed = true;
        return NULL;
    }
    return record_type;
}

/* Convert MCC type to ANVIL type */
anvil_type_t *codegen_type(mcc_codegen_t *cg, mcc_type_t *type)
{
    type = codegen_type_unwrap(type);
    if (!type) {
        mcc_error(cg->mcc_ctx,
                  "cannot lower an unresolved or missing C type");
        return NULL;
    }

    switch (type->kind) {
        case TYPE_VOID:
            return anvil_type_void(cg->anvil_ctx);
        case TYPE_CHAR:
            /* char signedness depends on is_unsigned flag */
            return type->is_unsigned ? anvil_type_u8(cg->anvil_ctx) 
                                     : anvil_type_i8(cg->anvil_ctx);
        case TYPE_SHORT:
            return type->is_unsigned ? anvil_type_u16(cg->anvil_ctx)
                                     : anvil_type_i16(cg->anvil_ctx);
        case TYPE_INT:
        case TYPE_ENUM:
            return type->is_unsigned ? anvil_type_u32(cg->anvil_ctx)
                                     : anvil_type_i32(cg->anvil_ctx);
        case TYPE_LONG: {
            /* The frontend's target DataLayout already selected ILP32/LP64.
             * Never infer this again from the host or a backend default. */
            if (type->size == anvil_type_size(anvil_type_i64(cg->anvil_ctx))) {
                return type->is_unsigned ? anvil_type_u64(cg->anvil_ctx)
                                         : anvil_type_i64(cg->anvil_ctx);
            }
            if (type->size == anvil_type_size(anvil_type_i32(cg->anvil_ctx))) {
                return type->is_unsigned ? anvil_type_u32(cg->anvil_ctx)
                                         : anvil_type_i32(cg->anvil_ctx);
            }
            mcc_error(cg->mcc_ctx, "unsupported target layout for C long");
            return NULL;
        }
        case TYPE_LONG_LONG:
            return type->is_unsigned ? anvil_type_u64(cg->anvil_ctx)
                                     : anvil_type_i64(cg->anvil_ctx);
        case TYPE_FLOAT:
            return anvil_type_f32(cg->anvil_ctx);
        case TYPE_DOUBLE:
            return anvil_type_f64(cg->anvil_ctx);
        case TYPE_LONG_DOUBLE:
            mcc_error(cg->mcc_ctx,
                      "long double ABI lowering is not implemented by MCC");
            return NULL;
        case TYPE_BOOL:
            return anvil_type_i1(cg->anvil_ctx);
        case TYPE_POINTER: {
            anvil_type_t *pointee = codegen_type(cg,
                type->data.pointer.pointee);
            if (!pointee) return NULL;
            return codegen_check_object_layout(cg, type,
                anvil_type_ptr(cg->anvil_ctx, pointee));
        }
        case TYPE_ARRAY: {
            anvil_type_t *element = codegen_type(cg, type->data.array.element);
            if (!element) return NULL;
            return codegen_check_object_layout(cg, type,
                anvil_type_array(cg->anvil_ctx, element,
                                 type->data.array.length));
        }
        case TYPE_STRUCT:
        case TYPE_UNION:
            return codegen_record_type(cg, type);
        case TYPE_FUNCTION: {
            anvil_type_t *ret_type = codegen_type(cg, type->data.function.return_type);
            if (!ret_type) return NULL;
            int num_params = type->data.function.num_params;
            if (num_params < 0 ||
                (size_t)num_params > SIZE_MAX / sizeof(anvil_type_t *)) {
                mcc_error(cg->mcc_ctx, "function parameter table overflow");
                return NULL;
            }
            anvil_type_t **param_types = mcc_alloc_array(cg->mcc_ctx,
                num_params > 0 ? (size_t)num_params : 1, sizeof(*param_types));
            if (!param_types) return NULL;
            
            int i = 0;
            for (mcc_func_param_t *p = type->data.function.params; p; p = p->next, i++) {
                if (i >= num_params) {
                    mcc_error(cg->mcc_ctx,
                              "function parameter count does not match list");
                    return NULL;
                }
                param_types[i] = codegen_param_type(cg, p->type);
                if (!param_types[i]) return NULL;
            }
            if (i != num_params) {
                mcc_error(cg->mcc_ctx,
                          "function parameter count does not match list");
                return NULL;
            }
            
            return anvil_type_func(cg->anvil_ctx, ret_type, param_types, num_params,
                                   type->data.function.is_variadic);
        }
        default:
            mcc_error(cg->mcc_ctx,
                      "code generation does not support type '%s'",
                      mcc_type_kind_name(type->kind));
            return NULL;
    }
}

/* Build an integer constant whose width and signedness match `anvil_type`.
 * Keep this aligned with the public ANVIL type API so verifier-visible
 * constants have the exact type expected by comparisons and stores. */
anvil_value_t *codegen_const_int_for_type(mcc_codegen_t *cg, anvil_type_t *anvil_type, int64_t val)
{
    if (!anvil_type || !anvil_type_is_integer(anvil_type)) {
        mcc_error(cg->mcc_ctx,
                  "integer constant requires a resolved Anvil integer type");
        return NULL;
    }
    if (anvil_type_is_bool(anvil_type))
        return anvil_const_i1(cg->anvil_ctx, val != 0);
    size_t sz = anvil_type_size(anvil_type);
    bool is_signed = anvil_type_is_signed(anvil_type);
    switch (sz) {
        case 1:
            return is_signed
                ? anvil_const_i8(cg->anvil_ctx, (int8_t)val)
                : anvil_const_u8(cg->anvil_ctx, (uint8_t)val);
        case 2:
            return is_signed
                ? anvil_const_i16(cg->anvil_ctx, (int16_t)val)
                : anvil_const_u16(cg->anvil_ctx, (uint16_t)val);
        case 4:
            return is_signed
                ? anvil_const_i32(cg->anvil_ctx, (int32_t)val)
                : anvil_const_u32(cg->anvil_ctx, (uint32_t)val);
        case 8:
            return is_signed
                ? anvil_const_i64(cg->anvil_ctx, val)
                : anvil_const_u64(cg->anvil_ctx, (uint64_t)val);
        default:
            mcc_error(cg->mcc_ctx,
                      "unsupported Anvil integer width for constant");
            return NULL;
    }
}
