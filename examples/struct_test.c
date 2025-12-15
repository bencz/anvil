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
 *   arch: x86, x86_64, s370, s370_xa, s390, zarch (default: s390)
 */

#include <anvil/anvil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    anvil_arch_t arch = ANVIL_ARCH_S390;
    const char *arch_name = "S/390";
    
    if (argc > 1) {
        if (strcmp(argv[1], "x86") == 0) {
            arch = ANVIL_ARCH_X86;
            arch_name = "x86";
        } else if (strcmp(argv[1], "x86_64") == 0) {
            arch = ANVIL_ARCH_X86_64;
            arch_name = "x86-64";
        } else if (strcmp(argv[1], "s370") == 0) {
            arch = ANVIL_ARCH_S370;
            arch_name = "S/370";
        } else if (strcmp(argv[1], "s370_xa") == 0) {
            arch = ANVIL_ARCH_S370_XA;
            arch_name = "S/370-XA";
        } else if (strcmp(argv[1], "s390") == 0) {
            arch = ANVIL_ARCH_S390;
            arch_name = "S/390";
        } else if (strcmp(argv[1], "zarch") == 0) {
            arch = ANVIL_ARCH_ZARCH;
            arch_name = "z/Architecture";
        } else {
            fprintf(stderr, "Unknown architecture: %s\n", argv[1]);
            fprintf(stderr, "Supported: x86, x86_64, s370, s370_xa, s390, zarch\n");
            return 1;
        }
    }
    
    printf("=== ANVIL Struct Example ===\n");
    printf("Target: %s\n\n", arch_name);
    
    anvil_ctx_t *ctx = anvil_ctx_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    anvil_ctx_set_target(ctx, arch);
    
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
