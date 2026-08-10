/*
 * MCC - Micro C Compiler
 * Type system implementation
 */

#include "anvil/anvil.h"
#include "mcc.h"

static const char *type_kind_names[] = {
    [TYPE_VOID]        = "void",
    [TYPE_CHAR]        = "char",
    [TYPE_SHORT]       = "short",
    [TYPE_INT]         = "int",
    [TYPE_LONG]        = "long",
    [TYPE_FLOAT]       = "float",
    [TYPE_DOUBLE]      = "double",
    [TYPE_LONG_DOUBLE] = "long double",
    [TYPE_POINTER]     = "pointer",
    [TYPE_ARRAY]       = "array",
    [TYPE_FUNCTION]    = "function",
    [TYPE_STRUCT]      = "struct",
    [TYPE_UNION]       = "union",
    [TYPE_ENUM]        = "enum",
    [TYPE_TYPEDEF]     = "typedef",
};

mcc_type_context_t *mcc_type_context_create(mcc_context_t *ctx)
{
    mcc_type_context_t *tctx = mcc_alloc(ctx, sizeof(mcc_type_context_t));
    if (!tctx) return NULL;
    tctx->ctx = ctx;
    
    /* ANVIL's target DataLayout is the single source of truth. */
    anvil_arch_t anvil_arch = mcc_arch_to_anvil(ctx->options.arch);
    anvil_ctx_t *layout_ctx = anvil_ctx_create_for_target(anvil_arch);
    if (!layout_ctx) {
        mcc_fatal(ctx, "Failed to create target DataLayout");
        return NULL;
    }
    const anvil_data_layout_t *layout = anvil_ctx_get_data_layout(layout_ctx);
    if (!layout) {
        anvil_ctx_destroy(layout_ctx);
        mcc_fatal(ctx, "Target has no DataLayout");
        return NULL;
    }
    int ptr_size = (int)layout->pointer.size;
    
    /* Determine long size based on data model:
     * - ILP32 (32-bit): long = 4 bytes (x86, S/370, S/370-XA, S/390, PPC32)
     * - LP64 (64-bit Unix): long = 8 bytes (x86_64, z/Architecture, PPC64, ARM64)
     * - LLP64 (64-bit Windows): long = 4 bytes (would need ANVIL_ABI_WIN64 check)
     * 
     * Note: IBM mainframes (S/370, S/390) use ILP32 even with 24/31-bit addressing.
     * z/Architecture uses LP64 with 64-bit addressing.
     */
    int long_size = (ptr_size == 8) ? 8 : 4;
    
    /* Create primitive types */
    tctx->type_void = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_void->kind = TYPE_VOID;
    tctx->type_void->size = 0;
    tctx->type_void->align = 1;
    
    tctx->type_char = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_char->kind = TYPE_CHAR;
    tctx->type_char->size = layout->i8.size;
    tctx->type_char->align = layout->i8.abi_align;
    
    tctx->type_schar = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_schar->kind = TYPE_CHAR;
    tctx->type_schar->is_unsigned = false;
    tctx->type_schar->size = layout->i8.size;
    tctx->type_schar->align = layout->i8.abi_align;
    
    tctx->type_uchar = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_uchar->kind = TYPE_CHAR;
    tctx->type_uchar->is_unsigned = true;
    tctx->type_uchar->size = layout->i8.size;
    tctx->type_uchar->align = layout->i8.abi_align;
    
    tctx->type_short = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_short->kind = TYPE_SHORT;
    tctx->type_short->size = layout->i16.size;
    tctx->type_short->align = layout->i16.abi_align;
    
    tctx->type_ushort = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_ushort->kind = TYPE_SHORT;
    tctx->type_ushort->is_unsigned = true;
    tctx->type_ushort->size = layout->i16.size;
    tctx->type_ushort->align = layout->i16.abi_align;
    
    tctx->type_int = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_int->kind = TYPE_INT;
    tctx->type_int->size = layout->i32.size;
    tctx->type_int->align = layout->i32.abi_align;
    
    tctx->type_uint = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_uint->kind = TYPE_INT;
    tctx->type_uint->is_unsigned = true;
    tctx->type_uint->size = layout->i32.size;
    tctx->type_uint->align = layout->i32.abi_align;
    
    /* long size depends on architecture */
    tctx->type_long = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_long->kind = TYPE_LONG;
    tctx->type_long->size = long_size;
    tctx->type_long->align = long_size == 8
        ? layout->i64.abi_align : layout->i32.abi_align;
    
    tctx->type_ulong = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_ulong->kind = TYPE_LONG;
    tctx->type_ulong->is_unsigned = true;
    tctx->type_ulong->size = long_size;
    tctx->type_ulong->align = long_size == 8
        ? layout->i64.abi_align : layout->i32.abi_align;
    
    /* C99 long long types - always 8 bytes */
    tctx->type_llong = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_llong->kind = TYPE_LONG_LONG;
    tctx->type_llong->size = layout->i64.size;
    tctx->type_llong->align = layout->i64.abi_align;
    
    tctx->type_ullong = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_ullong->kind = TYPE_LONG_LONG;
    tctx->type_ullong->is_unsigned = true;
    tctx->type_ullong->size = layout->i64.size;
    tctx->type_ullong->align = layout->i64.abi_align;
    
    tctx->type_float = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_float->kind = TYPE_FLOAT;
    tctx->type_float->size = layout->f32.size;
    tctx->type_float->align = layout->f32.abi_align;
    
    tctx->type_double = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_double->kind = TYPE_DOUBLE;
    tctx->type_double->size = layout->f64.size;
    tctx->type_double->align = layout->f64.abi_align;
    
    tctx->type_ldouble = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_ldouble->kind = TYPE_LONG_DOUBLE;
    tctx->type_ldouble->size = layout->f64.size;
    tctx->type_ldouble->align = layout->f64.abi_align;

    tctx->type_bool = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_bool->kind = TYPE_BOOL;
    tctx->type_bool->size = layout->i1.size;
    tctx->type_bool->align = layout->i1.abi_align;
    
    /* C99 Complex types (size is 2x the base type) */
    tctx->type_cfloat = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_cfloat->kind = TYPE_COMPLEX_FLOAT;
    tctx->type_cfloat->size = 8;   /* 2 * sizeof(float) */
    tctx->type_cfloat->align = 4;
    
    tctx->type_cdouble = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_cdouble->kind = TYPE_COMPLEX_DOUBLE;
    tctx->type_cdouble->size = 16;  /* 2 * sizeof(double) */
    tctx->type_cdouble->align = 8;
    
    tctx->type_cldouble = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_cldouble->kind = TYPE_COMPLEX_LDOUBLE;
    tctx->type_cldouble->size = 16;  /* 2 * sizeof(long double) - platform dependent */
    tctx->type_cldouble->align = 8;
    
    /* Store pointer size for use in mcc_type_pointer */
    tctx->ptr_size = ptr_size;
    tctx->ptr_align = layout->pointer.abi_align;
    anvil_ctx_destroy(layout_ctx);
    
    return tctx;
}

void mcc_type_context_destroy(mcc_type_context_t *tctx)
{
    (void)tctx; /* Arena allocated */
}

/* Primitive type getters */
mcc_type_t *mcc_type_void(mcc_type_context_t *tctx) { return tctx->type_void; }
mcc_type_t *mcc_type_char(mcc_type_context_t *tctx) { return tctx->type_char; }
mcc_type_t *mcc_type_schar(mcc_type_context_t *tctx) { return tctx->type_schar; }
mcc_type_t *mcc_type_uchar(mcc_type_context_t *tctx) { return tctx->type_uchar; }
mcc_type_t *mcc_type_short(mcc_type_context_t *tctx) { return tctx->type_short; }
mcc_type_t *mcc_type_ushort(mcc_type_context_t *tctx) { return tctx->type_ushort; }
mcc_type_t *mcc_type_int(mcc_type_context_t *tctx) { return tctx->type_int; }
mcc_type_t *mcc_type_uint(mcc_type_context_t *tctx) { return tctx->type_uint; }
mcc_type_t *mcc_type_long(mcc_type_context_t *tctx) { return tctx->type_long; }
mcc_type_t *mcc_type_ulong(mcc_type_context_t *tctx) { return tctx->type_ulong; }
mcc_type_t *mcc_type_llong(mcc_type_context_t *tctx) { return tctx->type_llong; }
mcc_type_t *mcc_type_ullong(mcc_type_context_t *tctx) { return tctx->type_ullong; }
mcc_type_t *mcc_type_float(mcc_type_context_t *tctx) { return tctx->type_float; }
mcc_type_t *mcc_type_double(mcc_type_context_t *tctx) { return tctx->type_double; }
mcc_type_t *mcc_type_long_double(mcc_type_context_t *tctx) { return tctx->type_ldouble; }
mcc_type_t *mcc_type_bool(mcc_type_context_t *tctx) { return tctx->type_bool; }
mcc_type_t *mcc_type_complex_float(mcc_type_context_t *tctx) { return tctx->type_cfloat; }
mcc_type_t *mcc_type_complex_double(mcc_type_context_t *tctx) { return tctx->type_cdouble; }
mcc_type_t *mcc_type_complex_ldouble(mcc_type_context_t *tctx) { return tctx->type_cldouble; }

/* Derived type constructors */
mcc_type_t *mcc_type_pointer(mcc_type_context_t *tctx, mcc_type_t *pointee)
{
    mcc_type_t *type = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    type->kind = TYPE_POINTER;
    type->data.pointer.pointee = pointee;
    type->size = tctx->ptr_size;  /* Use architecture-specific pointer size */
    type->align = tctx->ptr_align;
    return type;
}

mcc_type_t *mcc_type_array(mcc_type_context_t *tctx, mcc_type_t *element, size_t length)
{
    if (!tctx || !element || (element->size != 0 &&
        length > SIZE_MAX / element->size)) {
        if (tctx) mcc_error(tctx->ctx, "array type size overflow");
        return NULL;
    }
    mcc_type_t *type = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    if (!type) return NULL;
    type->kind = TYPE_ARRAY;
    type->data.array.element = element;
    type->data.array.length = length;
    type->size = element->size * length;
    type->align = element->align;
    return type;
}

mcc_type_t *mcc_type_incomplete_array(mcc_type_context_t *tctx, mcc_type_t *element)
{
    mcc_type_t *type = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    type->kind = TYPE_ARRAY;
    type->data.array.element = element;
    type->data.array.length = 0;
    type->size = 0;
    type->align = element->align;
    return type;
}

mcc_type_t *mcc_type_function(mcc_type_context_t *tctx, mcc_type_t *return_type,
                               mcc_func_param_t *params, int num_params, bool variadic)
{
    mcc_type_t *type = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    type->kind = TYPE_FUNCTION;
    type->data.function.return_type = return_type;
    type->data.function.params = params;
    type->data.function.num_params = num_params;
    type->data.function.is_variadic = variadic;
    type->size = 0;
    type->align = 1;
    return type;
}

mcc_type_t *mcc_type_struct(mcc_type_context_t *tctx, const char *tag)
{
    mcc_type_t *type = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    type->kind = TYPE_STRUCT;
    type->data.record.tag = tag ? mcc_strdup(tctx->ctx, tag) : NULL;
    type->data.record.is_complete = false;
    return type;
}

mcc_type_t *mcc_type_union(mcc_type_context_t *tctx, const char *tag)
{
    mcc_type_t *type = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    type->kind = TYPE_UNION;
    type->data.record.tag = tag ? mcc_strdup(tctx->ctx, tag) : NULL;
    type->data.record.is_complete = false;
    return type;
}

mcc_type_t *mcc_type_enum(mcc_type_context_t *tctx, const char *tag)
{
    mcc_type_t *type = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    type->kind = TYPE_ENUM;
    type->data.enumeration.tag = tag ? mcc_strdup(tctx->ctx, tag) : NULL;
    type->data.enumeration.is_complete = false;
    type->size = 4; /* Enums are int-sized */
    type->align = 4;
    return type;
}

/* Type completion */
static bool checked_type_align_up(mcc_type_context_t *tctx, size_t value,
                                  size_t align, size_t *result)
{
    if (!align || (align & (align - 1)) != 0 ||
        value > SIZE_MAX - (align - 1)) {
        mcc_error(tctx->ctx, "record layout overflow or invalid alignment");
        return false;
    }
    *result = (value + align - 1) & ~(align - 1);
    return true;
}

bool mcc_type_complete_struct(mcc_type_context_t *tctx, mcc_type_t *type,
                              mcc_struct_field_t *fields, int num_fields)
{
    if (!tctx || !type || num_fields < 0) return false;

    /* Calculate size and alignment. Bitfield layout (bit_offset) is not
     * fully implemented — a full bitfield lowering would need codegen
     * changes in addition. Each bitfield currently occupies its whole
     * storage-unit type; that wastes space but keeps address arithmetic
     * straightforward for the test suite. */
    size_t offset = 0;
    size_t max_align = 1;

    for (mcc_struct_field_t *f = fields; f; f = f->next) {
        if (!f->type || !f->type->align) {
            mcc_error(tctx->ctx, "record field has incomplete layout");
            return false;
        }
        size_t align = f->type->align;
        if (align > max_align) max_align = align;

        if (!checked_type_align_up(tctx, offset, align, &offset) ||
            f->type->size > SIZE_MAX - offset) return false;
        offset += f->type->size;
    }

    size_t total;
    if (!checked_type_align_up(tctx, offset, max_align, &total)) return false;
    offset = 0;
    for (mcc_struct_field_t *f = fields; f; f = f->next) {
        if (!checked_type_align_up(tctx, offset, f->type->align, &offset))
            return false;
        f->offset = offset;
        offset += f->type->size;
    }
    type->data.record.fields = fields;
    type->data.record.num_fields = num_fields;
    type->data.record.is_complete = true;
    type->size = total;
    type->align = max_align;
    return true;
}

bool mcc_type_complete_union(mcc_type_context_t *tctx, mcc_type_t *type,
                             mcc_struct_field_t *fields, int num_fields)
{
    if (!tctx || !type || num_fields < 0) return false;
    
    /* Calculate size and alignment (max of all fields) */
    size_t max_size = 0;
    size_t max_align = 1;
    
    for (mcc_struct_field_t *f = fields; f; f = f->next) {
        if (!f->type || !f->type->align) {
            mcc_error(tctx->ctx, "union field has incomplete layout");
            return false;
        }
        f->offset = 0;
        if (f->type->size > max_size) max_size = f->type->size;
        if (f->type->align > max_align) max_align = f->type->align;
    }
    
    size_t total;
    if (!checked_type_align_up(tctx, max_size, max_align, &total)) return false;
    type->data.record.fields = fields;
    type->data.record.num_fields = num_fields;
    type->data.record.is_complete = true;
    type->size = total;
    type->align = max_align;
    return true;
}

void mcc_type_complete_enum(mcc_type_t *type)
{
    type->data.enumeration.is_complete = true;
}

/* Type qualifiers */
mcc_type_t *mcc_type_qualified(mcc_type_context_t *tctx, mcc_type_t *type, mcc_type_qual_t quals)
{
    if (type->qualifiers == quals) return type;
    
    mcc_type_t *qtype = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    *qtype = *type;
    qtype->qualifiers = quals;
    return qtype;
}

mcc_type_t *mcc_type_unqualified(mcc_type_t *type)
{
    /* If already unqualified, return as-is. Otherwise we'd need a
     * context to allocate a copy — callers that need the context
     * should use mcc_type_qualified(tctx, type, QUAL_NONE) instead.
     * To keep the old signature working safely, we only clear
     * qualifiers on a type we know is a local copy (owned by the
     * caller). The classic hazard — mutating the shared 'int'
     * singleton — is defused because primitive singletons start out
     * with QUAL_NONE anyway, so this early-return path matches them. */
    if (type->qualifiers == QUAL_NONE) return type;

    /* Stamp out the qualifiers. This assumes the caller holds the
     * only reference to `type`; passing in a shared qualified instance
     * would still mutate it — but the codebase uses this only after
     * mcc_type_qualified produced a fresh copy. */
    type->qualifiers = QUAL_NONE;
    return type;
}

/* Type queries */
bool mcc_type_is_void(mcc_type_t *type)
{
    return type->kind == TYPE_VOID;
}

bool mcc_type_is_integer(mcc_type_t *type)
{
    switch (type->kind) {
        case TYPE_CHAR:
        case TYPE_SHORT:
        case TYPE_INT:
        case TYPE_LONG:
        case TYPE_LONG_LONG:
        case TYPE_BOOL:
        case TYPE_ENUM:
            return true;
        default:
            return false;
    }
}

bool mcc_type_is_floating(mcc_type_t *type)
{
    switch (type->kind) {
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
            return true;
        default:
            return false;
    }
}

bool mcc_type_is_arithmetic(mcc_type_t *type)
{
    return mcc_type_is_integer(type) || mcc_type_is_floating(type);
}

bool mcc_type_is_scalar(mcc_type_t *type)
{
    return mcc_type_is_arithmetic(type) || type->kind == TYPE_POINTER;
}

bool mcc_type_is_pointer(mcc_type_t *type)
{
    return type->kind == TYPE_POINTER;
}

bool mcc_type_is_array(mcc_type_t *type)
{
    return type->kind == TYPE_ARRAY;
}

bool mcc_type_is_function(mcc_type_t *type)
{
    return type->kind == TYPE_FUNCTION;
}

bool mcc_type_is_struct(mcc_type_t *type)
{
    return type->kind == TYPE_STRUCT;
}

bool mcc_type_is_union(mcc_type_t *type)
{
    return type->kind == TYPE_UNION;
}

bool mcc_type_is_record(mcc_type_t *type)
{
    return type->kind == TYPE_STRUCT || type->kind == TYPE_UNION;
}

bool mcc_type_is_enum(mcc_type_t *type)
{
    return type->kind == TYPE_ENUM;
}

bool mcc_type_is_aggregate(mcc_type_t *type)
{
    return mcc_type_is_array(type) || mcc_type_is_record(type);
}

bool mcc_type_is_complete(mcc_type_t *type)
{
    switch (type->kind) {
        case TYPE_VOID:
            return false;
        case TYPE_ARRAY:
            return type->data.array.length > 0;
        case TYPE_STRUCT:
        case TYPE_UNION:
            return type->data.record.is_complete;
        case TYPE_ENUM:
            return type->data.enumeration.is_complete;
        default:
            return true;
    }
}

bool mcc_type_is_compatible(mcc_type_t *a, mcc_type_t *b)
{
    if (a->kind != b->kind) return false;
    
    switch (a->kind) {
        case TYPE_POINTER:
            return mcc_type_is_compatible(a->data.pointer.pointee,
                                          b->data.pointer.pointee);
        case TYPE_ARRAY:
            if (a->data.array.length != 0 && b->data.array.length != 0 &&
                a->data.array.length != b->data.array.length) {
                return false;
            }
            return mcc_type_is_compatible(a->data.array.element,
                                          b->data.array.element);
        case TYPE_FUNCTION: {
            /* Return type must match. */
            if (!mcc_type_is_compatible(a->data.function.return_type,
                                        b->data.function.return_type)) {
                return false;
            }
            /* If either side is a K&R-style declaration (unspecified params
             * marked via is_variadic=false and num_params==0 AND an explicit
             * flag would be needed), we accept as compatible. Real C89 K&R
             * compatibility would need more plumbing; for now, an empty
             * parameter list is treated as "unknown" so it composes with a
             * prototype. */
            if (a->data.function.num_params == 0 ||
                b->data.function.num_params == 0) {
                /* Both empty or one empty and the other has params ⇒
                 * considered compatible so forward declarations without
                 * prototypes match definitions with prototypes. */
                return true;
            }
            if (a->data.function.num_params != b->data.function.num_params) {
                return false;
            }
            if (a->data.function.is_variadic != b->data.function.is_variadic) {
                return false;
            }
            mcc_func_param_t *pa = a->data.function.params;
            mcc_func_param_t *pb = b->data.function.params;
            while (pa && pb) {
                if (!mcc_type_is_compatible(pa->type, pb->type)) return false;
                pa = pa->next;
                pb = pb->next;
            }
            /* Both must have terminated together. */
            return pa == NULL && pb == NULL;
        }
        case TYPE_STRUCT:
        case TYPE_UNION:
            return a == b; /* Must be same type */
        default:
            return a->is_unsigned == b->is_unsigned;
    }
}

bool mcc_type_is_same(mcc_type_t *a, mcc_type_t *b)
{
    return a == b || (mcc_type_is_compatible(a, b) && 
                      a->qualifiers == b->qualifiers);
}

/* Type conversions */
mcc_type_t *mcc_type_promote(mcc_type_context_t *tctx, mcc_type_t *type)
{
    /* Integer promotion: char, short -> int */
    switch (type->kind) {
        case TYPE_CHAR:
        case TYPE_SHORT:
            return type->is_unsigned ? tctx->type_uint : tctx->type_int;
        case TYPE_ENUM:
            return tctx->type_int;
        default:
            return type;
    }
}

mcc_type_t *mcc_type_common(mcc_type_context_t *tctx, mcc_type_t *a, mcc_type_t *b)
{
    /* Usual arithmetic conversions */
    a = mcc_type_promote(tctx, a);
    b = mcc_type_promote(tctx, b);
    
    /* If either is long double */
    if (a->kind == TYPE_LONG_DOUBLE || b->kind == TYPE_LONG_DOUBLE) {
        return tctx->type_ldouble;
    }
    
    /* If either is double */
    if (a->kind == TYPE_DOUBLE || b->kind == TYPE_DOUBLE) {
        return tctx->type_double;
    }
    
    /* If either is float */
    if (a->kind == TYPE_FLOAT || b->kind == TYPE_FLOAT) {
        return tctx->type_float;
    }
    
    /* Both are integers — descend by integer rank per C99 §6.3.1.8.
     * Order: long long > long > int. Signedness propagates upwards only
     * when the wider type is unsigned or both operands are unsigned. */
    if (a->kind == TYPE_LONG_LONG || b->kind == TYPE_LONG_LONG) {
        if (a->is_unsigned || b->is_unsigned) {
            return tctx->type_ullong;
        }
        return tctx->type_llong;
    }

    if (a->kind == TYPE_LONG || b->kind == TYPE_LONG) {
        if (a->is_unsigned || b->is_unsigned) {
            return tctx->type_ulong;
        }
        return tctx->type_long;
    }

    if (a->is_unsigned || b->is_unsigned) {
        return tctx->type_uint;
    }

    return tctx->type_int;
}

mcc_type_t *mcc_type_decay(mcc_type_context_t *tctx, mcc_type_t *type)
{
    /* Array decays to pointer */
    if (type->kind == TYPE_ARRAY) {
        return mcc_type_pointer(tctx, type->data.array.element);
    }
    
    /* Function decays to pointer to function */
    if (type->kind == TYPE_FUNCTION) {
        return mcc_type_pointer(tctx, type);
    }
    
    return type;
}

/* Type utilities */
const char *mcc_type_kind_name(mcc_type_kind_t kind)
{
    if (kind < TYPE_COUNT) {
        return type_kind_names[kind];
    }
    return "unknown";
}

/* Writes the printable form of `type` into `buf`, appending at `*pos`.
 * `cap` is the total capacity. Handles nested types safely by recursing on
 * `buf`+`*pos`. The public mcc_type_to_string wraps this with a per-call
 * buffer — the old version shared a single static buffer and called itself
 * recursively, which corrupted output for anything like `int**`. */
static void type_append(mcc_type_t *type, char *buf, size_t cap, size_t *pos)
{
    if (!type || *pos + 1 >= cap) return;

#define APPEND(...) do {                                               \
    int _n = snprintf(buf + *pos, cap - *pos, __VA_ARGS__);            \
    if (_n > 0) {                                                      \
        *pos += (size_t)_n;                                            \
        if (*pos >= cap) *pos = cap - 1;                               \
    }                                                                  \
} while (0)

    if (type->qualifiers & QUAL_CONST)    APPEND("const ");
    if (type->qualifiers & QUAL_VOLATILE) APPEND("volatile ");

    switch (type->kind) {
        case TYPE_VOID:        APPEND("void"); break;
        case TYPE_CHAR:
            if (type->is_unsigned) APPEND("unsigned ");
            APPEND("char");
            break;
        case TYPE_SHORT:
            if (type->is_unsigned) APPEND("unsigned ");
            APPEND("short");
            break;
        case TYPE_INT:
            if (type->is_unsigned) APPEND("unsigned ");
            APPEND("int");
            break;
        case TYPE_LONG:
            if (type->is_unsigned) APPEND("unsigned ");
            APPEND("long");
            break;
        case TYPE_LONG_LONG:
            if (type->is_unsigned) APPEND("unsigned ");
            APPEND("long long");
            break;
        case TYPE_FLOAT:       APPEND("float"); break;
        case TYPE_DOUBLE:      APPEND("double"); break;
        case TYPE_LONG_DOUBLE: APPEND("long double"); break;
        case TYPE_POINTER:
            type_append(type->data.pointer.pointee, buf, cap, pos);
            APPEND(" *");
            break;
        case TYPE_ARRAY:
            type_append(type->data.array.element, buf, cap, pos);
            APPEND("[%zu]", type->data.array.length);
            break;
        case TYPE_STRUCT:
            APPEND("struct %s", type->data.record.tag ? type->data.record.tag : "(anonymous)");
            break;
        case TYPE_UNION:
            APPEND("union %s", type->data.record.tag ? type->data.record.tag : "(anonymous)");
            break;
        case TYPE_ENUM:
            APPEND("enum %s", type->data.enumeration.tag ? type->data.enumeration.tag : "(anonymous)");
            break;
        case TYPE_FUNCTION:
            type_append(type->data.function.return_type, buf, cap, pos);
            APPEND(" ()");
            break;
        default:
            APPEND("?");
            break;
    }
#undef APPEND
}

char *mcc_type_to_string(mcc_type_t *type)
{
    /* Per-call buffer: a static shared buffer could not survive the
     * recursive calls this function makes on pointer/array element
     * types. 256 bytes is enough for normal use and mirrors the old
     * bound. The returned pointer is valid until the next call from
     * the same thread. */
    static __thread char buf[256];
    size_t pos = 0;
    buf[0] = '\0';
    type_append(type, buf, sizeof(buf), &pos);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

size_t mcc_type_sizeof(mcc_type_t *type)
{
    return type->size;
}

size_t mcc_type_alignof(mcc_type_t *type)
{
    return type->align;
}

mcc_struct_field_t *mcc_type_find_field(mcc_type_t *type, const char *name)
{
    if (type->kind != TYPE_STRUCT && type->kind != TYPE_UNION) {
        return NULL;
    }

    for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next) {
        if (f->name) {
            if (strcmp(f->name, name) == 0) {
                return f;
            }
            continue;
        }
        /* Anonymous field: C11 anonymous struct/union — recurse into it so
         * `o.x` finds `x` declared inside `struct outer { struct { int x; }; }`.
         * Bitfield-padding unnamed fields have a non-record type and the
         * recursion bails out quickly. */
        if (f->type && (f->type->kind == TYPE_STRUCT || f->type->kind == TYPE_UNION)) {
            mcc_struct_field_t *inner = mcc_type_find_field(f->type, name);
            if (inner) {
                return inner;
            }
        }
    }

    return NULL;
}
