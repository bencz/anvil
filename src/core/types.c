/*
 * ANVIL - Type system implementation
 */

#include "anvil/anvil_internal.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

anvil_type_t *anvil_type_create(anvil_ctx_t *ctx, anvil_type_kind_t kind)
{
    if (!ctx) return NULL;
    anvil_type_t *type = anvil_ctx_calloc(ctx, 1, sizeof(anvil_type_t));
    if (!type) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM, "Out of memory creating type");
        return NULL;
    }
    type->kind = kind;
    type->owner_ctx = ctx;
    return type;
}

static anvil_type_t *type_register(anvil_ctx_t *ctx, anvil_type_t *type)
{
    if (!ctx || !type || type->owner_ctx != ctx) return NULL;
    type->ctx_next = ctx->types;
    ctx->types = type;
    return type;
}

/* Free a single type's owned allocations (strings, child arrays) but not the
 * type struct itself nor any referenced types. Used during anvil_ctx_destroy
 * where every type is walked exactly once. */
void anvil_type_free(anvil_type_t *type)
{
    if (!type) return;

    switch (type->kind) {
        case ANVIL_TYPE_STRUCT:
            free(type->data.struc.name);
            free(type->data.struc.fields);
            free(type->data.struc.offsets);
            break;
        case ANVIL_TYPE_FUNC:
            free(type->data.func.params);
            break;
        default:
            break;
    }
    free(type);
}

void anvil_type_init_sizes(anvil_ctx_t *ctx)
{
    if (!ctx) return;

    const anvil_data_layout_t *dl = &ctx->data_layout;

#define INIT_SCALAR(member, type_kind, entry, signedness)                       \
    do {                                                                        \
        if (!ctx->member) {                                                     \
            anvil_type_t *new_type = anvil_type_create(ctx, (type_kind));       \
            if (new_type) {                                                     \
                new_type->size = (entry).size;                                  \
                new_type->align = (entry).abi_align;                            \
                new_type->preferred_align = (entry).preferred_align;            \
                new_type->is_signed = (signedness);                             \
                ctx->member = type_register(ctx, new_type);                     \
            }                                                                   \
        } else {                                                                \
            ctx->member->size = (entry).size;                                   \
            ctx->member->align = (entry).abi_align;                             \
            ctx->member->preferred_align = (entry).preferred_align;             \
        }                                                                       \
    } while (0)

    anvil_layout_entry_t void_layout = { 0, 1, 1 };
    INIT_SCALAR(type_void, ANVIL_TYPE_VOID, void_layout, false);
    /* i1 is a one-bit integer with byte-addressable storage. */
    INIT_SCALAR(type_i1, ANVIL_TYPE_I1, dl->i1, false);
    INIT_SCALAR(type_i8, ANVIL_TYPE_I8, dl->i8, true);
    INIT_SCALAR(type_i16, ANVIL_TYPE_I16, dl->i16, true);
    INIT_SCALAR(type_i32, ANVIL_TYPE_I32, dl->i32, true);
    INIT_SCALAR(type_i64, ANVIL_TYPE_I64, dl->i64, true);
    INIT_SCALAR(type_u8, ANVIL_TYPE_U8, dl->i8, false);
    INIT_SCALAR(type_u16, ANVIL_TYPE_U16, dl->i16, false);
    INIT_SCALAR(type_u32, ANVIL_TYPE_U32, dl->i32, false);
    INIT_SCALAR(type_u64, ANVIL_TYPE_U64, dl->i64, false);
    INIT_SCALAR(type_f32, ANVIL_TYPE_F32, dl->f32, false);
    INIT_SCALAR(type_f64, ANVIL_TYPE_F64, dl->f64, false);

#undef INIT_SCALAR

    /* Cache i8* and void* pointer types — they dominate composite usage
     * (every string constant, every alloca-derived address, every byte
     * buffer). Caching avoids a calloc+free cycle per call and eliminates
     * what used to be the biggest composite-type leak. */
    if (!ctx->type_ptr_i8) {
        anvil_type_t *type = anvil_type_create(ctx, ANVIL_TYPE_PTR);
        if (type && ctx->type_i8) {
            type->size = dl->pointer.size;
            type->align = dl->pointer.abi_align;
            type->preferred_align = dl->pointer.preferred_align;
            type->data.pointee = ctx->type_i8;
            ctx->type_ptr_i8 = type_register(ctx, type);
        } else {
            anvil_type_free(type);
        }
    }
    if (ctx->type_ptr_i8) {
        ctx->type_ptr_i8->size = dl->pointer.size;
        ctx->type_ptr_i8->align = dl->pointer.abi_align;
        ctx->type_ptr_i8->preferred_align = dl->pointer.preferred_align;
        ctx->type_ptr_i8->data.pointee = ctx->type_i8;
    }
    if (!ctx->type_ptr_void) {
        anvil_type_t *type = anvil_type_create(ctx, ANVIL_TYPE_PTR);
        if (type && ctx->type_void) {
            type->size = dl->pointer.size;
            type->align = dl->pointer.abi_align;
            type->preferred_align = dl->pointer.preferred_align;
            type->data.pointee = ctx->type_void;
            ctx->type_ptr_void = type_register(ctx, type);
        } else {
            anvil_type_free(type);
        }
    }
    if (ctx->type_ptr_void) {
        ctx->type_ptr_void->size = dl->pointer.size;
        ctx->type_ptr_void->align = dl->pointer.abi_align;
        ctx->type_ptr_void->preferred_align = dl->pointer.preferred_align;
        ctx->type_ptr_void->data.pointee = ctx->type_void;
    }
}

anvil_type_t *anvil_type_void(anvil_ctx_t *ctx)
{
    return ctx ? ctx->type_void : NULL;
}

anvil_type_t *anvil_type_i1(anvil_ctx_t *ctx)
{
    return ctx ? ctx->type_i1 : NULL;
}

anvil_type_t *anvil_type_i8(anvil_ctx_t *ctx)
{
    return ctx ? ctx->type_i8 : NULL;
}

anvil_type_t *anvil_type_i16(anvil_ctx_t *ctx)
{
    return ctx ? ctx->type_i16 : NULL;
}

anvil_type_t *anvil_type_i32(anvil_ctx_t *ctx)
{
    return ctx ? ctx->type_i32 : NULL;
}

anvil_type_t *anvil_type_i64(anvil_ctx_t *ctx)
{
    return ctx ? ctx->type_i64 : NULL;
}

anvil_type_t *anvil_type_u8(anvil_ctx_t *ctx)
{
    return ctx ? ctx->type_u8 : NULL;
}

anvil_type_t *anvil_type_u16(anvil_ctx_t *ctx)
{
    return ctx ? ctx->type_u16 : NULL;
}

anvil_type_t *anvil_type_u32(anvil_ctx_t *ctx)
{
    return ctx ? ctx->type_u32 : NULL;
}

anvil_type_t *anvil_type_u64(anvil_ctx_t *ctx)
{
    return ctx ? ctx->type_u64 : NULL;
}

anvil_type_t *anvil_type_f32(anvil_ctx_t *ctx)
{
    return ctx ? ctx->type_f32 : NULL;
}

anvil_type_t *anvil_type_f64(anvil_ctx_t *ctx)
{
    return ctx ? ctx->type_f64 : NULL;
}

anvil_type_t *anvil_type_decimal(anvil_ctx_t *ctx,
                                  anvil_decimal_encoding_t encoding,
                                  unsigned precision,
                                  unsigned scale)
{
    if (!ctx || !ctx->target_configured || precision == 0 || scale > precision) {
        if (ctx && !ctx->target_configured)
            anvil_set_error(ctx, ANVIL_ERR_NO_TARGET,
                            "Select a target before creating types");
        return NULL;
    }
    if (encoding != ANVIL_DECIMAL_PACKED &&
        encoding != ANVIL_DECIMAL_ZONED) {
        return NULL;
    }
    if (encoding == ANVIL_DECIMAL_PACKED && precision > UINT_MAX - 2u) {
        return NULL;
    }

    anvil_type_t *type = anvil_type_create(ctx, ANVIL_TYPE_DECIMAL);
    if (!type) return NULL;

    type->align = 1;
    type->preferred_align = 1;
    type->data.decimal.encoding = encoding;
    type->data.decimal.precision = precision;
    type->data.decimal.scale = scale;

    if (encoding == ANVIL_DECIMAL_PACKED) {
        type->size = (precision + 2u) / 2u;
    } else {
        type->size = precision;
    }

    return type_register(ctx, type);
}

anvil_type_t *anvil_type_decimal_packed(anvil_ctx_t *ctx,
                                         unsigned precision,
                                         unsigned scale)
{
    return anvil_type_decimal(ctx, ANVIL_DECIMAL_PACKED, precision, scale);
}

anvil_type_t *anvil_type_decimal_zoned(anvil_ctx_t *ctx,
                                        unsigned precision,
                                        unsigned scale)
{
    return anvil_type_decimal(ctx, ANVIL_DECIMAL_ZONED, precision, scale);
}

anvil_type_t *anvil_type_ptr(anvil_ctx_t *ctx, anvil_type_t *pointee)
{
    if (!ctx || !ctx->target_configured || !pointee ||
        pointee->owner_ctx != ctx) {
        if (ctx && !ctx->target_configured)
            anvil_set_error(ctx, ANVIL_ERR_NO_TARGET,
                            "Select a target before creating types");
        return NULL;
    }
    /* Fast path: return the cached pointer type for the common cases. */
    if (pointee == ctx->type_i8 && ctx->type_ptr_i8) {
        anvil_ctx_freeze_target(ctx);
        return ctx->type_ptr_i8;
    }
    if (pointee == ctx->type_void && ctx->type_ptr_void) {
        anvil_ctx_freeze_target(ctx);
        return ctx->type_ptr_void;
    }

    anvil_type_t *type = anvil_type_create(ctx, ANVIL_TYPE_PTR);
    if (!type) return NULL;

    type->size = ctx->data_layout.pointer.size;
    type->align = ctx->data_layout.pointer.abi_align;
    type->preferred_align = ctx->data_layout.pointer.preferred_align;
    type->data.pointee = pointee;

    type_register(ctx, type);
    anvil_ctx_freeze_target(ctx);
    return type;
}

static bool checked_align_up(size_t value, size_t align, size_t *result)
{
    if (!result || align == 0 || (align & (align - 1)) != 0 ||
        value > SIZE_MAX - (align - 1)) {
        return false;
    }
    *result = (value + align - 1) & ~(align - 1);
    return true;
}

static uint64_t struct_name_hash(const char *name)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    while (*name) {
        hash ^= (unsigned char)*name++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool named_table_reserve(anvil_ctx_t *ctx, size_t needed)
{
    size_t old_cap = ctx->named_struct_bucket_count;
    if (old_cap && needed <= old_cap - old_cap / 4) return true;
    size_t new_cap = old_cap ? old_cap * 2 : 32;
    if (new_cap < old_cap || new_cap > SIZE_MAX / sizeof(anvil_type_t *)) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM, "Named struct table overflow");
        return false;
    }
    anvil_type_t **buckets = anvil_ctx_calloc(ctx, new_cap, sizeof(*buckets));
    if (!buckets) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                        "Out of memory growing named struct table");
        return false;
    }
    for (size_t i = 0; i < old_cap; i++) {
        anvil_type_t *cur = ctx->named_struct_buckets[i];
        while (cur) {
            anvil_type_t *next = cur->data.struc.symbol_next;
            size_t slot = (size_t)(struct_name_hash(cur->data.struc.name) &
                                   (uint64_t)(new_cap - 1));
            cur->data.struc.symbol_next = buckets[slot];
            buckets[slot] = cur;
            cur = next;
        }
    }
    free(ctx->named_struct_buckets);
    ctx->named_struct_buckets = buckets;
    ctx->named_struct_bucket_count = new_cap;
    return true;
}

anvil_type_t *anvil_type_named_struct(anvil_ctx_t *ctx, const char *name)
{
    if (!ctx || !ctx->target_configured || !name || !*name) {
        if (ctx && !ctx->target_configured) {
            anvil_set_error(ctx, ANVIL_ERR_NO_TARGET,
                            "Select a target before creating types");
            return NULL;
        }
        if (ctx) anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                                 "Identified struct requires a non-empty name");
        return NULL;
    }
    if (ctx->named_struct_bucket_count) {
        size_t slot = (size_t)(struct_name_hash(name) &
                               (uint64_t)(ctx->named_struct_bucket_count - 1));
        for (anvil_type_t *cur = ctx->named_struct_buckets[slot]; cur;
             cur = cur->data.struc.symbol_next) {
            if (strcmp(cur->data.struc.name, name) == 0) {
                anvil_ctx_freeze_target(ctx);
                return cur;
            }
        }
    }

    anvil_type_t *type = anvil_type_create(ctx, ANVIL_TYPE_STRUCT);
    if (!type) return NULL;
    type->data.struc.name = anvil_ctx_strdup(ctx, name);
    if (!type->data.struc.name ||
        !named_table_reserve(ctx, ctx->named_struct_count + 1)) {
        anvil_type_free(type);
        return NULL;
    }
    type->data.struc.identified = true;
    type->data.struc.complete = false;
    size_t slot = (size_t)(struct_name_hash(name) &
                           (uint64_t)(ctx->named_struct_bucket_count - 1));
    type->data.struc.symbol_next = ctx->named_struct_buckets[slot];
    ctx->named_struct_buckets[slot] = type;
    ctx->named_struct_count++;
    type_register(ctx, type);
    anvil_ctx_freeze_target(ctx);
    return type;
}

bool anvil_type_struct_set_body(anvil_type_t *type, anvil_type_t **fields,
                                size_t num_fields, bool packed)
{
    if (!type || type->kind != ANVIL_TYPE_STRUCT ||
        !type->data.struc.identified || type->data.struc.complete) {
        if (type && type->owner_ctx)
            anvil_set_error(type->owner_ctx, ANVIL_ERR_INVALID_TYPE,
                            "Struct body may only be set once on an opaque identified struct");
        return false;
    }
    anvil_ctx_t *ctx = type->owner_ctx;
    if (!ctx || !ctx->target_configured) {
        if (ctx) anvil_set_error(ctx, ANVIL_ERR_NO_TARGET,
                                 "Select a target before defining types");
        return false;
    }
    if ((num_fields && !fields) ||
        num_fields > SIZE_MAX / sizeof(*fields) ||
        num_fields > SIZE_MAX / sizeof(size_t)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG, "Invalid struct body");
        return false;
    }
    for (size_t i = 0; i < num_fields; i++) {
        if (!fields[i] || fields[i]->owner_ctx != ctx ||
            fields[i]->kind == ANVIL_TYPE_VOID ||
            fields[i]->kind == ANVIL_TYPE_FUNC || fields[i]->align == 0) {
            anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE,
                            "Struct fields must be sized first-class types from the same context");
            return false;
        }
    }

    anvil_type_t **new_fields = NULL;
    size_t *new_offsets = NULL;
    if (num_fields) {
        new_fields = anvil_ctx_malloc(ctx, num_fields * sizeof(*new_fields));
        new_offsets = anvil_ctx_malloc(ctx, num_fields * sizeof(*new_offsets));
        if (!new_fields || !new_offsets) {
            free(new_fields);
            free(new_offsets);
            anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                            "Out of memory defining struct body");
            return false;
        }
    }

    size_t offset = 0;
    size_t max_align = packed ? 1 : ctx->data_layout.aggregate_abi_align;
    size_t max_preferred = packed ? 1
                                  : ctx->data_layout.aggregate_preferred_align;
    for (size_t i = 0; i < num_fields; i++) {
        size_t field_align = packed ? 1 : fields[i]->align;
        if (!checked_align_up(offset, field_align, &offset) ||
            fields[i]->size > SIZE_MAX - offset) {
            free(new_fields);
            free(new_offsets);
            anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE,
                            "Struct layout overflows size_t");
            return false;
        }
        new_fields[i] = fields[i];
        new_offsets[i] = offset;
        offset += fields[i]->size;
        if (field_align > max_align) max_align = field_align;
        if (fields[i]->preferred_align > max_preferred)
            max_preferred = fields[i]->preferred_align;
    }
    size_t total = offset;
    if (!packed && !checked_align_up(offset, max_align, &total)) {
        free(new_fields);
        free(new_offsets);
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE,
                        "Struct tail padding overflows size_t");
        return false;
    }

    type->data.struc.fields = new_fields;
    type->data.struc.offsets = new_offsets;
    type->data.struc.num_fields = num_fields;
    type->data.struc.packed = packed;
    type->data.struc.complete = true;
    type->size = total;
    type->align = packed ? 1 : max_align;
    type->preferred_align = packed ? 1 : max_preferred;
    return true;
}

anvil_type_t *anvil_type_literal_struct(anvil_ctx_t *ctx,
                                         anvil_type_t **fields,
                                         size_t num_fields, bool packed)
{
    if (!ctx || !ctx->target_configured) {
        if (ctx) anvil_set_error(ctx, ANVIL_ERR_NO_TARGET,
                                 "Select a target before creating types");
        return NULL;
    }
    anvil_type_t *type = anvil_type_create(ctx, ANVIL_TYPE_STRUCT);
    if (!type) return NULL;
    type->data.struc.identified = true; /* reuse transactional body builder */
    if (!anvil_type_struct_set_body(type, fields, num_fields, packed)) {
        anvil_type_free(type);
        return NULL;
    }
    type->data.struc.identified = false;
    type_register(ctx, type);
    anvil_ctx_freeze_target(ctx);
    return type;
}

anvil_type_t *anvil_type_struct(anvil_ctx_t *ctx, const char *name,
                                 anvil_type_t **fields, size_t num_fields)
{
    if (!name) return anvil_type_literal_struct(ctx, fields, num_fields, false);
    anvil_type_t *type = anvil_type_named_struct(ctx, name);
    if (!type || !anvil_type_struct_set_body(type, fields, num_fields, false))
        return NULL;
    return type;
}

anvil_type_t *anvil_type_array(anvil_ctx_t *ctx, anvil_type_t *elem, size_t count)
{
    if (!ctx || !ctx->target_configured || !elem || elem->owner_ctx != ctx || elem->align == 0 ||
        elem->kind == ANVIL_TYPE_VOID || elem->kind == ANVIL_TYPE_FUNC ||
        (count != 0 && elem->size > SIZE_MAX / count)) {
        if (ctx && !ctx->target_configured)
            anvil_set_error(ctx, ANVIL_ERR_NO_TARGET,
                            "Select a target before creating types");
        return NULL;
    }

    anvil_type_t *type = anvil_type_create(ctx, ANVIL_TYPE_ARRAY);
    if (!type) return NULL;

    type->data.array.elem = elem;
    type->data.array.count = count;
    type->size = elem->size * count;
    type->align = elem->align;
    type->preferred_align = elem->preferred_align;
    
    type_register(ctx, type);
    anvil_ctx_freeze_target(ctx);
    return type;
}

bool anvil_cc_resolve(const anvil_ctx_t *ctx, anvil_cc_t requested,
                      anvil_cc_t *effective)
{
    if (effective) *effective = ANVIL_CC_DEFAULT;
    if (!ctx || !ctx->target_configured || !effective ||
        (unsigned)requested > (unsigned)ANVIL_CC_MVS) {
        return false;
    }

    anvil_abi_t abi = ctx->abi;
    switch (ctx->arch) {
        case ANVIL_ARCH_X86:
            if (abi != ANVIL_ABI_DEFAULT && abi != ANVIL_ABI_SYSV &&
                abi != ANVIL_ABI_DARWIN && abi != ANVIL_ABI_WIN64)
                return false; /* WIN64 names the repository's COFF platform
                                 selector for both x86 widths. */
            if (requested == ANVIL_CC_DEFAULT || requested == ANVIL_CC_CDECL)
                *effective = ANVIL_CC_CDECL;
            else if (requested == ANVIL_CC_STDCALL ||
                     requested == ANVIL_CC_FASTCALL)
                *effective = requested;
            else
                return false;
            return true;

        case ANVIL_ARCH_X86_64:
            if (abi == ANVIL_ABI_WIN64) {
                if (requested != ANVIL_CC_DEFAULT &&
                    requested != ANVIL_CC_CDECL &&
                    requested != ANVIL_CC_WIN64) return false;
                *effective = ANVIL_CC_WIN64;
                return true;
            }
            if (abi != ANVIL_ABI_DEFAULT && abi != ANVIL_ABI_SYSV &&
                abi != ANVIL_ABI_DARWIN) return false;
            if (requested != ANVIL_CC_DEFAULT &&
                requested != ANVIL_CC_CDECL &&
                requested != ANVIL_CC_SYSV) return false;
            *effective = ANVIL_CC_SYSV;
            return true;

        case ANVIL_ARCH_ARM64:
        case ANVIL_ARCH_PPC32:
        case ANVIL_ARCH_PPC64:
        case ANVIL_ARCH_PPC64LE:
            if (abi != ANVIL_ABI_DEFAULT && abi != ANVIL_ABI_SYSV &&
                abi != ANVIL_ABI_DARWIN) return false;
            if (requested != ANVIL_CC_DEFAULT &&
                requested != ANVIL_CC_CDECL &&
                requested != ANVIL_CC_SYSV) return false;
            *effective = ANVIL_CC_SYSV;
            return true;

        case ANVIL_ARCH_S370:
        case ANVIL_ARCH_S370_XA:
        case ANVIL_ARCH_S390:
        case ANVIL_ARCH_ZARCH:
            if (abi != ANVIL_ABI_DEFAULT && abi != ANVIL_ABI_MVS)
                return false;
            if (requested != ANVIL_CC_DEFAULT && requested != ANVIL_CC_MVS)
                return false;
            *effective = ANVIL_CC_MVS;
            return true;

        case ANVIL_ARCH_NONE:
        case ANVIL_ARCH_COUNT:
            return false;
    }
    return false;
}

anvil_type_t *anvil_type_func_cc(anvil_ctx_t *ctx, anvil_type_t *ret,
                                  anvil_type_t **params, size_t num_params,
                                  bool variadic, anvil_cc_t cc)
{
    anvil_cc_t effective_cc = ANVIL_CC_DEFAULT;
    if (!ctx) return NULL;
    if (!ctx->target_configured) {
        anvil_set_error(ctx, ANVIL_ERR_NO_TARGET,
                        "Select a target before creating types");
        return NULL;
    }
    if ((ret && (ret->owner_ctx != ctx || ret->kind == ANVIL_TYPE_FUNC ||
                 (ret->kind != ANVIL_TYPE_VOID &&
                  !anvil_sem_type_is_sized(ret))))) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE,
                        "Function return type must be void or a sized type from its context");
        return NULL;
    }
    if ((num_params > 0 && !params)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                        "Function parameter array is missing");
        return NULL;
    }
    if (num_params > SIZE_MAX / sizeof(anvil_type_t *)) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                        "Function parameter array size overflows size_t");
        return NULL;
    }
    if (!anvil_cc_resolve(ctx, cc, &effective_cc)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                        "Calling convention is incompatible with the selected target/ABI");
        return NULL;
    }
    if (variadic && (effective_cc == ANVIL_CC_STDCALL ||
                     effective_cc == ANVIL_CC_FASTCALL)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                        "Variadic x86 functions require the caller-cleanup CDECL convention");
        return NULL;
    }
    for (size_t i = 0; i < num_params; i++) {
        if (!params[i] || params[i]->owner_ctx != ctx ||
            params[i]->kind == ANVIL_TYPE_VOID ||
            params[i]->kind == ANVIL_TYPE_FUNC ||
            !anvil_sem_type_is_sized(params[i])) {
            anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE,
                            "Function parameter must be a sized non-function type from its context");
            return NULL;
        }
    }

    anvil_type_t *type = anvil_type_create(ctx, ANVIL_TYPE_FUNC);
    if (!type) return NULL;

    type->data.func.ret = ret ? ret : ctx->type_void;
    type->data.func.num_params = num_params;
    type->data.func.variadic = variadic;
    type->data.func.cc = effective_cc;
    
    if (num_params > 0) {
        type->data.func.params = anvil_ctx_calloc(ctx, num_params,
                                                  sizeof(anvil_type_t *));
        if (!type->data.func.params) {
            anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                            "Out of memory creating function type");
            anvil_type_free(type);
            return NULL;
        }
        memcpy(type->data.func.params, params, num_params * sizeof(anvil_type_t *));
    }
    
    /* Function types don't have a meaningful size */
    type->size = 0;
    type->align = 1;
    type->preferred_align = 1;
    
    type_register(ctx, type);
    anvil_ctx_freeze_target(ctx);
    return type;
}

anvil_type_t *anvil_type_func(anvil_ctx_t *ctx, anvil_type_t *ret,
                               anvil_type_t **params, size_t num_params,
                               bool variadic)
{
    return anvil_type_func_cc(ctx, ret, params, num_params, variadic,
                              ANVIL_CC_DEFAULT);
}

anvil_cc_t anvil_type_func_cc_value(const anvil_type_t *type)
{
    return type && type->kind == ANVIL_TYPE_FUNC
               ? type->data.func.cc : ANVIL_CC_DEFAULT;
}

typedef struct type_pair {
    const anvil_type_t *lhs;
    const anvil_type_t *rhs;
    const struct type_pair *parent;
} type_pair_t;

static bool types_equal_graph(const anvil_type_t *lhs,
                              const anvil_type_t *rhs,
                              const type_pair_t *parents)
{
    if (lhs == rhs) return true;
    if (!lhs || !rhs || lhs->owner_ctx != rhs->owner_ctx ||
        lhs->kind != rhs->kind) {
        return false;
    }
    for (const type_pair_t *p = parents; p; p = p->parent) {
        if (p->lhs == lhs && p->rhs == rhs) return true;
    }
    type_pair_t pair = { lhs, rhs, parents };

    switch (lhs->kind) {
        case ANVIL_TYPE_VOID:
        case ANVIL_TYPE_I1:
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U8:
        case ANVIL_TYPE_U16:
        case ANVIL_TYPE_U32:
        case ANVIL_TYPE_U64:
        case ANVIL_TYPE_F32:
        case ANVIL_TYPE_F64:
            return true;
        case ANVIL_TYPE_DECIMAL:
            return lhs->data.decimal.encoding == rhs->data.decimal.encoding &&
                   lhs->data.decimal.precision == rhs->data.decimal.precision &&
                   lhs->data.decimal.scale == rhs->data.decimal.scale;
        case ANVIL_TYPE_PTR:
            return types_equal_graph(lhs->data.pointee, rhs->data.pointee,
                                     &pair);
        case ANVIL_TYPE_ARRAY:
            return lhs->data.array.count == rhs->data.array.count &&
                   types_equal_graph(lhs->data.array.elem, rhs->data.array.elem,
                                     &pair);
        case ANVIL_TYPE_STRUCT:
            if (lhs->data.struc.identified || rhs->data.struc.identified)
                return false; /* distinct identified types are nominal */
            if (!lhs->data.struc.complete || !rhs->data.struc.complete)
                return false;
            if (lhs->data.struc.num_fields != rhs->data.struc.num_fields ||
                lhs->data.struc.packed != rhs->data.struc.packed) {
                return false;
            }
            for (size_t i = 0; i < lhs->data.struc.num_fields; i++) {
                if (!types_equal_graph(lhs->data.struc.fields[i],
                                       rhs->data.struc.fields[i], &pair)) {
                    return false;
                }
            }
            return true;
        case ANVIL_TYPE_FUNC:
            if (lhs->data.func.num_params != rhs->data.func.num_params ||
                lhs->data.func.variadic != rhs->data.func.variadic ||
                lhs->data.func.cc != rhs->data.func.cc ||
                !types_equal_graph(lhs->data.func.ret, rhs->data.func.ret,
                                   &pair)) {
                return false;
            }
            for (size_t i = 0; i < lhs->data.func.num_params; i++) {
                if (!types_equal_graph(lhs->data.func.params[i],
                                       rhs->data.func.params[i], &pair)) {
                    return false;
                }
            }
            return true;
    }
    return false;
}

bool anvil_types_equal(const anvil_type_t *lhs, const anvil_type_t *rhs)
{
    return types_equal_graph(lhs, rhs, NULL);
}

bool anvil_gep_analyze_step(anvil_type_t **current,
                            const anvil_value_t *index,
                            size_t index_ordinal,
                            anvil_gep_step_t *step)
{
    if (!current || !*current || !index || !step ||
        !anvil_type_is_integer(index->type) ||
        index->type->kind == ANVIL_TYPE_I1) return false;
    anvil_type_t *type = *current;
    if (index_ordinal == 0) {
        step->kind = ANVIL_GEP_STEP_SCALE;
        step->amount = type->size;
        step->result_type = type;
        return true;
    }
    if (type->kind == ANVIL_TYPE_ARRAY) {
        type = type->data.array.elem;
        step->kind = ANVIL_GEP_STEP_SCALE;
        step->amount = type->size;
        step->result_type = type;
        *current = type;
        return true;
    }
    if (type->kind == ANVIL_TYPE_STRUCT && type->data.struc.complete &&
        index->kind == ANVIL_VAL_CONST_INT && index->data.i >= 0 &&
        (size_t)index->data.i < type->data.struc.num_fields) {
        size_t field = (size_t)index->data.i;
        step->kind = ANVIL_GEP_STEP_FIELD_OFFSET;
        step->amount = type->data.struc.offsets[field];
        type = type->data.struc.fields[field];
        step->result_type = type;
        *current = type;
        return true;
    }
    return false;
}

bool anvil_gep_const_step_offset(const anvil_gep_step_t *step,
                                 const anvil_value_t *index,
                                 int64_t *offset)
{
    if (!step || !index || !offset || step->amount > (size_t)INT64_MAX)
        return false;
    if (step->kind == ANVIL_GEP_STEP_FIELD_OFFSET) {
        *offset = (int64_t)step->amount;
        return true;
    }
    if (index->kind != ANVIL_VAL_CONST_INT) return false;
    unsigned ptr_bits = index->owner_ctx
        ? (unsigned)(index->owner_ctx->data_layout.pointer.size * 8) : 64;
    if (ptr_bits == 0 || ptr_bits > 64) return false;
    uint64_t bits = index->data.u;
    if (ptr_bits < 64) bits &= (UINT64_C(1) << ptr_bits) - 1;
    int64_t signed_index;
    if (index->type && !index->type->is_signed) {
        if (bits > (uint64_t)INT64_MAX) return false;
        signed_index = (int64_t)bits;
    } else {
        if (ptr_bits < 64 && (bits & (UINT64_C(1) << (ptr_bits - 1))))
            bits |= ~((UINT64_C(1) << ptr_bits) - 1);
        signed_index = (int64_t)bits;
    }
    return !__builtin_mul_overflow(signed_index, (int64_t)step->amount,
                                   offset);
}

bool anvil_gep_accumulate_offset(int64_t *total, int64_t step_offset)
{
    if (!total) return false;
    int64_t result;
    if (__builtin_add_overflow(*total, step_offset, &result)) return false;
    *total = result;
    return true;
}

size_t anvil_type_size(anvil_type_t *type)
{
    return type ? type->size : 0;
}

unsigned anvil_type_bit_width(const anvil_type_t *type)
{
    if (!type) return 0;
    switch (type->kind) {
        case ANVIL_TYPE_I1: return 1;
        case ANVIL_TYPE_I8: case ANVIL_TYPE_U8: return 8;
        case ANVIL_TYPE_I16: case ANVIL_TYPE_U16: return 16;
        case ANVIL_TYPE_I32: case ANVIL_TYPE_U32:
        case ANVIL_TYPE_F32: return 32;
        case ANVIL_TYPE_I64: case ANVIL_TYPE_U64:
        case ANVIL_TYPE_F64: return 64;
        case ANVIL_TYPE_PTR:
            return type->size <= UINT_MAX / 8 ? (unsigned)(type->size * 8) : 0;
        default:
            return 0;
    }
}

size_t anvil_type_align(anvil_type_t *type)
{
    return type ? type->align : 1;
}

size_t anvil_type_preferred_align(anvil_type_t *type)
{
    return type ? type->preferred_align : 1;
}

bool anvil_type_struct_is_identified(const anvil_type_t *type)
{
    return type && type->kind == ANVIL_TYPE_STRUCT &&
           type->data.struc.identified;
}

bool anvil_type_struct_is_opaque(const anvil_type_t *type)
{
    return type && type->kind == ANVIL_TYPE_STRUCT &&
           !type->data.struc.complete;
}

bool anvil_type_struct_is_packed(const anvil_type_t *type)
{
    return type && type->kind == ANVIL_TYPE_STRUCT &&
           type->data.struc.complete && type->data.struc.packed;
}

const char *anvil_type_struct_name(const anvil_type_t *type)
{
    return type && type->kind == ANVIL_TYPE_STRUCT
               ? type->data.struc.name : NULL;
}

size_t anvil_type_struct_field_count(const anvil_type_t *type)
{
    return type && type->kind == ANVIL_TYPE_STRUCT &&
           type->data.struc.complete ? type->data.struc.num_fields : 0;
}

anvil_type_t *anvil_type_struct_field_type(const anvil_type_t *type,
                                            size_t field_idx)
{
    return type && type->kind == ANVIL_TYPE_STRUCT &&
           type->data.struc.complete &&
           field_idx < type->data.struc.num_fields
               ? type->data.struc.fields[field_idx] : NULL;
}

size_t anvil_type_struct_field_offset(const anvil_type_t *type,
                                      size_t field_idx)
{
    return type && type->kind == ANVIL_TYPE_STRUCT &&
           type->data.struc.complete &&
           field_idx < type->data.struc.num_fields
               ? type->data.struc.offsets[field_idx] : SIZE_MAX;
}

anvil_decimal_encoding_t anvil_type_decimal_encoding(anvil_type_t *type)
{
    if (!type || type->kind != ANVIL_TYPE_DECIMAL) {
        return ANVIL_DECIMAL_PACKED;
    }
    return type->data.decimal.encoding;
}

unsigned anvil_type_decimal_precision(anvil_type_t *type)
{
    if (!type || type->kind != ANVIL_TYPE_DECIMAL) return 0;
    return type->data.decimal.precision;
}

unsigned anvil_type_decimal_scale(anvil_type_t *type)
{
    if (!type || type->kind != ANVIL_TYPE_DECIMAL) return 0;
    return type->data.decimal.scale;
}

bool anvil_type_is_bool(anvil_type_t *type)
{
    return type && type->kind == ANVIL_TYPE_I1;
}

bool anvil_type_is_integer(anvil_type_t *type)
{
    if (!type) return false;
    switch (type->kind) {
        case ANVIL_TYPE_I1:
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U8:
        case ANVIL_TYPE_U16:
        case ANVIL_TYPE_U32:
        case ANVIL_TYPE_U64:
            return true;
        default:
            return false;
    }
}

bool anvil_type_is_floating(anvil_type_t *type)
{
    return type && (type->kind == ANVIL_TYPE_F32 ||
                    type->kind == ANVIL_TYPE_F64);
}

bool anvil_type_is_signed(anvil_type_t *type)
{
    return type ? type->is_signed : false;
}

bool anvil_type_is_pointer(anvil_type_t *type)
{
    return type && type->kind == ANVIL_TYPE_PTR;
}

bool anvil_sem_bool_type(const anvil_type_t *type)
{
    return type && type->kind == ANVIL_TYPE_I1;
}

bool anvil_sem_type_is_sized(const anvil_type_t *type)
{
    if (!type || type->kind == ANVIL_TYPE_VOID ||
        type->kind == ANVIL_TYPE_FUNC) return false;
    if (type->kind == ANVIL_TYPE_STRUCT)
        return type->data.struc.complete;
    if (type->kind == ANVIL_TYPE_ARRAY)
        return anvil_sem_type_is_sized(type->data.array.elem);
    return true;
}

bool anvil_sem_binary_types(anvil_op_t op, const anvil_type_t *lhs,
                            const anvil_type_t *rhs,
                            const anvil_type_t *result)
{
    if (!lhs || !rhs || !result || !anvil_types_equal(lhs, rhs) ||
        !anvil_types_equal(lhs, result)) return false;
    switch (op) {
        case ANVIL_OP_ADD: case ANVIL_OP_SUB: case ANVIL_OP_MUL:
            return anvil_type_is_integer((anvil_type_t *)lhs) &&
                   lhs->kind != ANVIL_TYPE_I1;
        case ANVIL_OP_AND: case ANVIL_OP_OR: case ANVIL_OP_XOR:
            return anvil_type_is_integer((anvil_type_t *)lhs);
        case ANVIL_OP_SDIV: case ANVIL_OP_SMOD:
            return anvil_type_is_integer((anvil_type_t *)lhs) &&
                   lhs->kind != ANVIL_TYPE_I1 && lhs->is_signed;
        case ANVIL_OP_UDIV: case ANVIL_OP_UMOD:
            return anvil_type_is_integer((anvil_type_t *)lhs) &&
                   lhs->kind != ANVIL_TYPE_I1 && !lhs->is_signed;
        case ANVIL_OP_SHL: case ANVIL_OP_SHR: case ANVIL_OP_SAR:
            /* Shift amount may have any integer width, so it is handled by
             * the builder/verifier separately. */
            return false;
        case ANVIL_OP_FADD: case ANVIL_OP_FSUB:
        case ANVIL_OP_FMUL: case ANVIL_OP_FDIV:
            return anvil_type_is_floating((anvil_type_t *)lhs);
        default:
            return false;
    }
}

bool anvil_sem_unary_types(anvil_op_t op, const anvil_type_t *operand,
                           const anvil_type_t *result)
{
    if (!operand || !result || !anvil_types_equal(operand, result)) return false;
    if (op == ANVIL_OP_NEG)
        return anvil_type_is_integer((anvil_type_t *)operand) &&
               operand->kind != ANVIL_TYPE_I1;
    if (op == ANVIL_OP_NOT)
        return anvil_type_is_integer((anvil_type_t *)operand);
    if (op == ANVIL_OP_FNEG || op == ANVIL_OP_FABS)
        return anvil_type_is_floating((anvil_type_t *)operand);
    return false;
}

bool anvil_sem_cmp_types(anvil_op_t op, const anvil_type_t *lhs,
                         const anvil_type_t *rhs,
                         const anvil_type_t *result)
{
    if (!lhs || !rhs || !result || !anvil_types_equal(lhs, rhs) ||
        !anvil_sem_bool_type(result)) return false;
    if (op == ANVIL_OP_FCMP)
        return anvil_type_is_floating((anvil_type_t *)lhs);
    if (op < ANVIL_OP_CMP_EQ || op > ANVIL_OP_CMP_UGE) return false;
    if (op == ANVIL_OP_CMP_EQ || op == ANVIL_OP_CMP_NE)
        return anvil_type_is_integer((anvil_type_t *)lhs) ||
               lhs->kind == ANVIL_TYPE_PTR;
    if (!anvil_type_is_integer((anvil_type_t *)lhs)) return false;
    if (lhs->kind == ANVIL_TYPE_I1) return false;
    if (op >= ANVIL_OP_CMP_ULT) return !lhs->is_signed;
    return lhs->is_signed;
}

bool anvil_sem_cast_types(anvil_op_t op, const anvil_type_t *source,
                          const anvil_type_t *result)
{
    if (!source || !result) return false;
    bool src_int = anvil_type_is_integer((anvil_type_t *)source);
    bool dst_int = anvil_type_is_integer((anvil_type_t *)result);
    bool src_fp = anvil_type_is_floating((anvil_type_t *)source);
    bool dst_fp = anvil_type_is_floating((anvil_type_t *)result);
    unsigned src_bits = anvil_type_bit_width(source);
    unsigned dst_bits = anvil_type_bit_width(result);
    switch (op) {
        case ANVIL_OP_TRUNC:
            return src_int && dst_int && src_bits > dst_bits;
        case ANVIL_OP_ZEXT: case ANVIL_OP_SEXT:
            return src_int && dst_int && src_bits < dst_bits;
        case ANVIL_OP_FPTRUNC:
            return src_fp && dst_fp && source->size > result->size;
        case ANVIL_OP_FPEXT:
            return src_fp && dst_fp && source->size < result->size;
        case ANVIL_OP_FPTOSI: case ANVIL_OP_FPTOUI:
            return src_fp && dst_int &&
                   (op == ANVIL_OP_FPTOSI ? result->is_signed
                                          : !result->is_signed);
        case ANVIL_OP_SITOFP: case ANVIL_OP_UITOFP:
            return src_int && dst_fp &&
                   (op == ANVIL_OP_SITOFP ? source->is_signed
                                          : !source->is_signed);
        case ANVIL_OP_PTRTOINT:
            return source->kind == ANVIL_TYPE_PTR && dst_int;
        case ANVIL_OP_INTTOPTR:
            return src_int && result->kind == ANVIL_TYPE_PTR;
        case ANVIL_OP_BITCAST:
        {
            bool src_scalar = src_int || src_fp ||
                              source->kind == ANVIL_TYPE_PTR;
            bool dst_scalar = dst_int || dst_fp ||
                              result->kind == ANVIL_TYPE_PTR;
            return src_scalar && dst_scalar && src_bits != 0 &&
                   src_bits == dst_bits;
        }
        default:
            return false;
    }
}

anvil_type_t *anvil_sem_memory_object_type(const anvil_value_t *value)
{
    if (!value || !value->type) return NULL;
    if (value->kind == ANVIL_VAL_GLOBAL &&
        value->type->kind != ANVIL_TYPE_FUNC) return value->type;
    return value->type->kind == ANVIL_TYPE_PTR
               ? value->type->data.pointee : NULL;
}

anvil_type_t *anvil_sem_callee_func_type(const anvil_value_t *callee)
{
    if (!callee || !callee->type) return NULL;
    if (callee->type->kind == ANVIL_TYPE_FUNC) return callee->type;
    if (callee->type->kind == ANVIL_TYPE_PTR &&
        callee->type->data.pointee &&
        callee->type->data.pointee->kind == ANVIL_TYPE_FUNC)
        return callee->type->data.pointee;
    return NULL;
}
