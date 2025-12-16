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
