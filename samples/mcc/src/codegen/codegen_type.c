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

bool codegen_type_pass_by_reference(mcc_type_t *type)
{
    type = codegen_type_unwrap(type);
    return type && (type->kind == TYPE_STRUCT || type->kind == TYPE_UNION);
}

anvil_type_t *codegen_param_type(mcc_codegen_t *cg, mcc_type_t *type)
{
    anvil_type_t *value_type = codegen_type(cg, type);
    if (codegen_type_pass_by_reference(type)) {
        return anvil_type_ptr(cg->anvil_ctx, value_type);
    }
    return value_type;
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
            /* `long` is LP64 on most modern Unix targets (64-bit) but
             * ILP32 on Windows and 32-bit systems. Pick based on the
             * target arch's pointer size. */
            const anvil_arch_info_t *ai = anvil_ctx_get_arch_info(cg->anvil_ctx);
            bool lp64 = ai && ai->ptr_size == 8;
            if (lp64) {
                return type->is_unsigned ? anvil_type_u64(cg->anvil_ctx)
                                         : anvil_type_i64(cg->anvil_ctx);
            }
            return type->is_unsigned ? anvil_type_u32(cg->anvil_ctx)
                                     : anvil_type_i32(cg->anvil_ctx);
        }
        case TYPE_LONG_LONG:
            return type->is_unsigned ? anvil_type_u64(cg->anvil_ctx)
                                     : anvil_type_i64(cg->anvil_ctx);
        case TYPE_FLOAT:
            return anvil_type_f32(cg->anvil_ctx);
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
            return anvil_type_f64(cg->anvil_ctx);
        case TYPE_BOOL:
            return anvil_type_i1(cg->anvil_ctx);
        case TYPE_POINTER:
            return anvil_type_ptr(cg->anvil_ctx,
                codegen_type(cg, type->data.pointer.pointee));
        case TYPE_ARRAY:
            return anvil_type_array(cg->anvil_ctx,
                codegen_type(cg, type->data.array.element),
                type->data.array.length);
        case TYPE_STRUCT:
        case TYPE_UNION: {
            if (type->anvil_cached) return type->anvil_cached;

            char record_name[64];
            snprintf(record_name, sizeof(record_name), "mcc.%s.%p",
                     type->kind == TYPE_UNION ? "union" : "struct",
                     (void *)type);
            anvil_type_t *record_type = anvil_type_named_struct(
                cg->anvil_ctx, record_name);
            if (!record_type) return NULL;
            type->anvil_cached = record_type;
            if (!type->data.record.is_complete) return record_type;

            /* Anonymous record members occupy physical storage. Only unnamed
             * non-record bitfields are padding. */
            int num_named_fields = 0;
            for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next) {
                if (f->name || (f->type && mcc_type_is_record(f->type) &&
                                f->bitfield_width == 0)) num_named_fields++;
            }

            if (num_named_fields < 0 ||
                (size_t)num_named_fields > SIZE_MAX / sizeof(anvil_type_t *)) {
                mcc_error(cg->mcc_ctx, "record field table size overflow");
                return NULL;
            }
            anvil_type_t **field_types = mcc_alloc(cg->mcc_ctx,
                (num_named_fields > 0 ? (size_t)num_named_fields : 1) *
                sizeof(anvil_type_t *));
            if (!field_types) return NULL;

            int i = 0;
            for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next) {
                if (f->name || (f->type && mcc_type_is_record(f->type) &&
                                f->bitfield_width == 0)) {
                    field_types[i++] = codegen_type(cg, f->type);
                    if (!field_types[i - 1]) return NULL;
                }
            }

            if (type->kind == TYPE_UNION && num_named_fields > 0) {
                anvil_type_t *anchor = field_types[0];
                size_t max_size = anvil_type_size(anchor);
                size_t max_align = anvil_type_align(anchor);
                for (int field = 1; field < num_named_fields; field++) {
                    size_t field_size = anvil_type_size(field_types[field]);
                    size_t field_align = anvil_type_align(field_types[field]);
                    if (field_size > max_size) max_size = field_size;
                    if (field_align > max_align) {
                        max_align = field_align;
                        anchor = field_types[field];
                    }
                }
                field_types[0] = anchor;
                size_t anchor_size = anvil_type_size(anchor);
                size_t storage_fields = 1;
                if (anchor_size < max_size) {
                    field_types[1] = anvil_type_array(cg->anvil_ctx,
                        anvil_type_u8(cg->anvil_ctx), max_size - anchor_size);
                    if (!field_types[1]) return NULL;
                    storage_fields = 2;
                }
                if (!anvil_type_struct_set_body(record_type, field_types,
                                                storage_fields, false)) {
                    mcc_error(cg->mcc_ctx, "failed to lower union layout: %s",
                              anvil_ctx_get_error(cg->anvil_ctx));
                    return NULL;
                }
                return record_type;
            }

            if (!anvil_type_struct_set_body(record_type, field_types,
                                            (size_t)num_named_fields, false)) {
                mcc_error(cg->mcc_ctx, "failed to lower struct layout: %s",
                          anvil_ctx_get_error(cg->anvil_ctx));
                return NULL;
            }
            return record_type;
        }
        case TYPE_FUNCTION: {
            anvil_type_t *ret_type = codegen_type(cg, type->data.function.return_type);
            int num_params = type->data.function.num_params;
            anvil_type_t **param_types = mcc_alloc(cg->mcc_ctx,
                (num_params > 0 ? num_params : 1) * sizeof(anvil_type_t*));
            
            int i = 0;
            for (mcc_func_param_t *p = type->data.function.params; p; p = p->next, i++) {
                param_types[i] = codegen_param_type(cg, p->type);
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

/* Get sizeof for a type using ANVIL arch info for pointer size */
size_t codegen_sizeof(mcc_codegen_t *cg, mcc_type_t *type)
{
    if (!type) {
        mcc_error(cg->mcc_ctx, "sizeof requires a resolved C type");
        return 0;
    }
    anvil_type_t *lowered = codegen_type(cg, type);
    if (!lowered) return 0;
    return anvil_type_size(lowered);
}
