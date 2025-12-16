/*
 * MCC - Micro C Compiler
 * Code Generator - Type Conversion
 * 
 * This file handles conversion from MCC types to ANVIL types.
 */

#include "codegen_internal.h"

/* Convert MCC type to ANVIL type */
anvil_type_t *codegen_type(mcc_codegen_t *cg, mcc_type_t *type)
{
    if (!type) return anvil_type_i32(cg->anvil_ctx);
    
    switch (type->kind) {
        case TYPE_VOID:
            return anvil_type_void(cg->anvil_ctx);
        case TYPE_CHAR:
            return anvil_type_i8(cg->anvil_ctx);
        case TYPE_SHORT:
            return anvil_type_i16(cg->anvil_ctx);
        case TYPE_INT:
        case TYPE_ENUM:
            return anvil_type_i32(cg->anvil_ctx);
        case TYPE_LONG:
            return anvil_type_i32(cg->anvil_ctx); /* 32-bit long for C89 */
        case TYPE_FLOAT:
            return anvil_type_f32(cg->anvil_ctx);
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
            return anvil_type_f64(cg->anvil_ctx);
        case TYPE_POINTER:
            return anvil_type_ptr(cg->anvil_ctx,
                codegen_type(cg, type->data.pointer.pointee));
        case TYPE_ARRAY:
            return anvil_type_array(cg->anvil_ctx,
                codegen_type(cg, type->data.array.element),
                type->data.array.length);
        case TYPE_STRUCT:
        case TYPE_UNION: {
            /* Create struct type */
            int num_fields = type->data.record.num_fields;
            anvil_type_t **field_types = mcc_alloc(cg->mcc_ctx,
                num_fields * sizeof(anvil_type_t*));
            
            int i = 0;
            for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next, i++) {
                field_types[i] = codegen_type(cg, f->type);
            }
            
            return anvil_type_struct(cg->anvil_ctx, NULL, field_types, num_fields);
        }
        case TYPE_FUNCTION: {
            anvil_type_t *ret_type = codegen_type(cg, type->data.function.return_type);
            int num_params = type->data.function.num_params;
            anvil_type_t **param_types = mcc_alloc(cg->mcc_ctx,
                (num_params > 0 ? num_params : 1) * sizeof(anvil_type_t*));
            
            int i = 0;
            for (mcc_func_param_t *p = type->data.function.params; p; p = p->next, i++) {
                param_types[i] = codegen_type(cg, p->type);
            }
            
            return anvil_type_func(cg->anvil_ctx, ret_type, param_types, num_params,
                                   type->data.function.is_variadic);
        }
        default:
            return anvil_type_i32(cg->anvil_ctx);
    }
}

/* Get sizeof for a type using ANVIL arch info for pointer size */
size_t codegen_sizeof(mcc_codegen_t *cg, mcc_type_t *type)
{
    if (!type) return 0;
    
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(cg->anvil_ctx);
    int ptr_size = arch ? arch->ptr_size : 8;
    
    switch (type->kind) {
        case TYPE_POINTER:
            return ptr_size;
        
        case TYPE_LONG:
            /* long size depends on data model: LP64 = 8, ILP32 = 4 */
            return (ptr_size == 8) ? 8 : 4;
        
        case TYPE_ARRAY:
            return codegen_sizeof(cg, type->data.array.element) * type->data.array.length;
        
        case TYPE_STRUCT: {
            /* Recalculate struct size with correct pointer sizes */
            size_t offset = 0;
            size_t max_align = 1;
            
            for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next) {
                size_t field_size = codegen_sizeof(cg, f->type);
                size_t field_align = field_size < 8 ? field_size : 8;
                if (f->type->kind == TYPE_POINTER) field_align = ptr_size;
                
                if (field_align > max_align) max_align = field_align;
                offset = (offset + field_align - 1) & ~(field_align - 1);
                offset += field_size;
            }
            return (offset + max_align - 1) & ~(max_align - 1);
        }
        
        case TYPE_UNION: {
            size_t max_size = 0;
            for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next) {
                size_t field_size = codegen_sizeof(cg, f->type);
                if (field_size > max_size) max_size = field_size;
            }
            return max_size;
        }
        
        default:
            /* For basic types, use the size from mcc_type */
            return type->size;
    }
}
