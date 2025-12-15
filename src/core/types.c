/*
 * ANVIL - Type system implementation
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>

anvil_type_t *anvil_type_create(anvil_ctx_t *ctx, anvil_type_kind_t kind)
{
    anvil_type_t *type = calloc(1, sizeof(anvil_type_t));
    if (!type) return NULL;
    type->kind = kind;
    return type;
}

void anvil_type_init_sizes(anvil_ctx_t *ctx)
{
    if (!ctx) return;
    
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(ctx);
    int ptr_size = arch ? arch->ptr_size : 8;
    
    /* Create void type */
    if (!ctx->type_void) {
        ctx->type_void = anvil_type_create(ctx, ANVIL_TYPE_VOID);
        ctx->type_void->size = 0;
        ctx->type_void->align = 1;
    }
    
    /* Create i8 type */
    if (!ctx->type_i8) {
        ctx->type_i8 = anvil_type_create(ctx, ANVIL_TYPE_I8);
        ctx->type_i8->size = 1;
        ctx->type_i8->align = 1;
        ctx->type_i8->is_signed = true;
    }
    
    /* Create i16 type */
    if (!ctx->type_i16) {
        ctx->type_i16 = anvil_type_create(ctx, ANVIL_TYPE_I16);
        ctx->type_i16->size = 2;
        ctx->type_i16->align = 2;
        ctx->type_i16->is_signed = true;
    }
    
    /* Create i32 type */
    if (!ctx->type_i32) {
        ctx->type_i32 = anvil_type_create(ctx, ANVIL_TYPE_I32);
        ctx->type_i32->size = 4;
        ctx->type_i32->align = 4;
        ctx->type_i32->is_signed = true;
    }
    
    /* Create i64 type */
    if (!ctx->type_i64) {
        ctx->type_i64 = anvil_type_create(ctx, ANVIL_TYPE_I64);
        ctx->type_i64->size = 8;
        ctx->type_i64->align = 8;
        ctx->type_i64->is_signed = true;
    }
    
    /* Create u8 type */
    if (!ctx->type_u8) {
        ctx->type_u8 = anvil_type_create(ctx, ANVIL_TYPE_U8);
        ctx->type_u8->size = 1;
        ctx->type_u8->align = 1;
        ctx->type_u8->is_signed = false;
    }
    
    /* Create u16 type */
    if (!ctx->type_u16) {
        ctx->type_u16 = anvil_type_create(ctx, ANVIL_TYPE_U16);
        ctx->type_u16->size = 2;
        ctx->type_u16->align = 2;
        ctx->type_u16->is_signed = false;
    }
    
    /* Create u32 type */
    if (!ctx->type_u32) {
        ctx->type_u32 = anvil_type_create(ctx, ANVIL_TYPE_U32);
        ctx->type_u32->size = 4;
        ctx->type_u32->align = 4;
        ctx->type_u32->is_signed = false;
    }
    
    /* Create u64 type */
    if (!ctx->type_u64) {
        ctx->type_u64 = anvil_type_create(ctx, ANVIL_TYPE_U64);
        ctx->type_u64->size = 8;
        ctx->type_u64->align = 8;
        ctx->type_u64->is_signed = false;
    }
    
    /* Create f32 type */
    if (!ctx->type_f32) {
        ctx->type_f32 = anvil_type_create(ctx, ANVIL_TYPE_F32);
        ctx->type_f32->size = 4;
        ctx->type_f32->align = 4;
    }
    
    /* Create f64 type */
    if (!ctx->type_f64) {
        ctx->type_f64 = anvil_type_create(ctx, ANVIL_TYPE_F64);
        ctx->type_f64->size = 8;
        ctx->type_f64->align = 8;
    }
}

anvil_type_t *anvil_type_void(anvil_ctx_t *ctx)
{
    return ctx ? ctx->type_void : NULL;
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

anvil_type_t *anvil_type_ptr(anvil_ctx_t *ctx, anvil_type_t *pointee)
{
    if (!ctx) return NULL;
    
    anvil_type_t *type = anvil_type_create(ctx, ANVIL_TYPE_PTR);
    if (!type) return NULL;
    
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(ctx);
    type->size = arch ? arch->ptr_size : 8;
    type->align = type->size;
    type->data.pointee = pointee;
    
    return type;
}

anvil_type_t *anvil_type_struct(anvil_ctx_t *ctx, const char *name,
                                 anvil_type_t **fields, size_t num_fields)
{
    if (!ctx) return NULL;
    
    anvil_type_t *type = anvil_type_create(ctx, ANVIL_TYPE_STRUCT);
    if (!type) return NULL;
    
    type->data.struc.name = name ? strdup(name) : NULL;
    type->data.struc.num_fields = num_fields;
    type->data.struc.packed = false;
    
    if (num_fields > 0) {
        type->data.struc.fields = calloc(num_fields, sizeof(anvil_type_t *));
        type->data.struc.offsets = calloc(num_fields, sizeof(size_t));
        
        if (!type->data.struc.fields || !type->data.struc.offsets) {
            free(type->data.struc.fields);
            free(type->data.struc.offsets);
            free(type);
            return NULL;
        }
        
        /* Calculate offsets and total size */
        size_t offset = 0;
        size_t max_align = 1;
        
        for (size_t i = 0; i < num_fields; i++) {
            type->data.struc.fields[i] = fields[i];
            
            size_t field_align = fields[i]->align;
            if (field_align > max_align) max_align = field_align;
            
            /* Align offset */
            offset = (offset + field_align - 1) & ~(field_align - 1);
            type->data.struc.offsets[i] = offset;
            offset += fields[i]->size;
        }
        
        /* Final size with alignment padding */
        type->size = (offset + max_align - 1) & ~(max_align - 1);
        type->align = max_align;
    }
    
    return type;
}

anvil_type_t *anvil_type_array(anvil_ctx_t *ctx, anvil_type_t *elem, size_t count)
{
    if (!ctx || !elem) return NULL;
    
    anvil_type_t *type = anvil_type_create(ctx, ANVIL_TYPE_ARRAY);
    if (!type) return NULL;
    
    type->data.array.elem = elem;
    type->data.array.count = count;
    type->size = elem->size * count;
    type->align = elem->align;
    
    return type;
}

anvil_type_t *anvil_type_func(anvil_ctx_t *ctx, anvil_type_t *ret,
                               anvil_type_t **params, size_t num_params, bool variadic)
{
    if (!ctx) return NULL;
    
    anvil_type_t *type = anvil_type_create(ctx, ANVIL_TYPE_FUNC);
    if (!type) return NULL;
    
    type->data.func.ret = ret ? ret : ctx->type_void;
    type->data.func.num_params = num_params;
    type->data.func.variadic = variadic;
    
    if (num_params > 0 && params) {
        type->data.func.params = calloc(num_params, sizeof(anvil_type_t *));
        if (!type->data.func.params) {
            free(type);
            return NULL;
        }
        memcpy(type->data.func.params, params, num_params * sizeof(anvil_type_t *));
    }
    
    /* Function types don't have a meaningful size */
    type->size = 0;
    type->align = 1;
    
    return type;
}

size_t anvil_type_size(anvil_type_t *type)
{
    return type ? type->size : 0;
}

size_t anvil_type_align(anvil_type_t *type)
{
    return type ? type->align : 1;
}
