/*
 * ANVIL Struct Example
 * 
 * Demonstrates struct field access using STRUCT_GEP
 * 
 * Equivalent C code:
 *   struct Point {
 *       int x;
 *       int y;
 *   };
 *   
 *   int get_x(struct Point *p) {
 *       return p->x;
 *   }
 *   
 *   int get_y(struct Point *p) {
 *       return p->y;
 *   }
 *   
 *   int distance_squared(struct Point *p) {
 *       return p->x * p->x + p->y * p->y;
 *   }
 * 
 * Usage: struct_test [arch]
 *   arch: x86, x86_64, s370, s370_xa, s390, zarch, ppc32, ppc64, ppc64le, arm64
 */

#include <anvil/anvil.h>
#include <stdio.h>
#include <stdlib.h>
#include "arch_select.h"

int main(int argc, char **argv)
{
    anvil_ctx_t *ctx;
    arch_config_t config;
    
    EXAMPLE_SETUP(argc, argv, ctx, config, "ANVIL Struct Example");
    
    anvil_module_t *mod = anvil_module_create(ctx, "struct_test");
    if (!mod) {
        fprintf(stderr, "Failed to create module\n");
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    /* Types */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    
    /* Define struct Point { int x; int y; } */
    anvil_type_t *point_fields[] = { i32, i32 };
    anvil_type_t *point_type = anvil_type_struct(ctx, "Point", point_fields, 2);
    anvil_type_t *point_ptr = anvil_type_ptr(ctx, point_type);
    
    printf("Struct Point { int x; int y; }\n");
    printf("  Field 0 (x): int at offset 0\n");
    printf("  Field 1 (y): int at offset 4\n\n");
    
    /* ============================================
     * Function 1: int get_x(struct Point *p)
     * ============================================ */
    {
        anvil_type_t *params[] = { point_ptr };
        anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 1, false);
        anvil_func_t *func = anvil_func_create(mod, "get_x", func_type, ANVIL_LINK_EXTERNAL);
        
        anvil_block_t *entry = anvil_func_get_entry(func);
        anvil_set_insert_point(ctx, entry);
        
        anvil_value_t *p = anvil_func_get_param(func, 0);
        
        /* Get pointer to p->x (field 0) */
        anvil_value_t *x_ptr = anvil_build_struct_gep(ctx, point_type, p, 0, "x_ptr");
        anvil_value_t *x_val = anvil_build_load(ctx, i32, x_ptr, "x_val");
        
        anvil_build_ret(ctx, x_val);
    }
    
    /* ============================================
     * Function 2: int get_y(struct Point *p)
     * ============================================ */
    {
        anvil_type_t *params[] = { point_ptr };
        anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 1, false);
        anvil_func_t *func = anvil_func_create(mod, "get_y", func_type, ANVIL_LINK_EXTERNAL);
        
        anvil_block_t *entry = anvil_func_get_entry(func);
        anvil_set_insert_point(ctx, entry);
        
        anvil_value_t *p = anvil_func_get_param(func, 0);
        
        /* Get pointer to p->y (field 1) */
        anvil_value_t *y_ptr = anvil_build_struct_gep(ctx, point_type, p, 1, "y_ptr");
        anvil_value_t *y_val = anvil_build_load(ctx, i32, y_ptr, "y_val");
        
        anvil_build_ret(ctx, y_val);
    }
    
    /* ============================================
     * Function 3: int distance_squared(struct Point *p)
     * Returns: p->x * p->x + p->y * p->y
     * 
     * NOTE: We use stack temporaries to preserve intermediate
     * results across multiple operations. This is necessary
     * because the backend uses a limited set of registers.
     * ============================================ */
    {
        anvil_type_t *params[] = { point_ptr };
        anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 1, false);
        anvil_func_t *func = anvil_func_create(mod, "dist_sq", func_type, ANVIL_LINK_EXTERNAL);
        
        anvil_block_t *entry = anvil_func_get_entry(func);
        anvil_set_insert_point(ctx, entry);
        
        anvil_value_t *p = anvil_func_get_param(func, 0);
        
        /* Allocate temporaries on stack to preserve intermediate results */
        anvil_value_t *x_sq_tmp = anvil_build_alloca(ctx, i32, "x_sq_tmp");
        anvil_value_t *y_sq_tmp = anvil_build_alloca(ctx, i32, "y_sq_tmp");
        
        /* Load p->x and compute x*x */
        anvil_value_t *x_ptr = anvil_build_struct_gep(ctx, point_type, p, 0, "x_ptr");
        anvil_value_t *x = anvil_build_load(ctx, i32, x_ptr, "x");
        anvil_value_t *x_sq = anvil_build_mul(ctx, x, x, "x_sq");
        anvil_build_store(ctx, x_sq, x_sq_tmp);  /* Save x*x to stack */
        
        /* Load p->y and compute y*y */
        anvil_value_t *y_ptr = anvil_build_struct_gep(ctx, point_type, p, 1, "y_ptr");
        anvil_value_t *y = anvil_build_load(ctx, i32, y_ptr, "y");
        anvil_value_t *y_sq = anvil_build_mul(ctx, y, y, "y_sq");
        anvil_build_store(ctx, y_sq, y_sq_tmp);  /* Save y*y to stack */
        
        /* Load back and sum: x*x + y*y */
        anvil_value_t *x_sq_val = anvil_build_load(ctx, i32, x_sq_tmp, "x_sq_val");
        anvil_value_t *y_sq_val = anvil_build_load(ctx, i32, y_sq_tmp, "y_sq_val");
        anvil_value_t *result = anvil_build_add(ctx, x_sq_val, y_sq_val, "result");
        
        anvil_build_ret(ctx, result);
    }
    
    /* Generate code */
    char *output = NULL;
    size_t len = 0;
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    
    if (err != ANVIL_OK) {
        fprintf(stderr, "Code generation failed: %d\n", err);
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    printf("Generated %zu bytes of assembly:\n", len);
    printf("----------------------------------------\n");
    printf("%s\n", output);
    
    free(output);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    
    return 0;
}
