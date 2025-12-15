/*
 * ANVIL String Support Test
 * Tests string constant generation for all architectures
 * 
 * Usage: string_test [arch]
 *   arch: x86, x86_64, s370, s370_xa, s390, zarch, ppc32, ppc64, ppc64le, arm64
 */

#include <anvil/anvil.h>
#include <stdio.h>
#include <stdlib.h>
#include "arch_select.h"

static void test_strings(anvil_ctx_t *ctx)
{
    anvil_module_t *mod = anvil_module_create(ctx, "strtest");
    
    /* Create function type: char* get_message(void) */
    anvil_type_t *i8 = anvil_type_i8(ctx);
    anvil_type_t *ptr_i8 = anvil_type_ptr(ctx, i8);
    anvil_type_t *func_type = anvil_type_func(ctx, ptr_i8, NULL, 0, false);
    
    /* Create function */
    anvil_func_t *func = anvil_func_create(mod, "get_msg", func_type, ANVIL_LINK_EXTERNAL);
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    /* Create string constant and return it */
    anvil_value_t *str = anvil_const_string(ctx, "Hello, World!");
    anvil_build_ret(ctx, str);
    
    /* Create another function that uses multiple strings */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32 };
    anvil_type_t *func_type2 = anvil_type_func(ctx, ptr_i8, params, 1, false);
    
    anvil_func_t *func2 = anvil_func_create(mod, "get_str", func_type2, ANVIL_LINK_EXTERNAL);
    
    anvil_block_t *entry2 = anvil_func_get_entry(func2);
    anvil_set_insert_point(ctx, entry2);
    
    /* Just return a different string */
    anvil_value_t *str2 = anvil_const_string(ctx, "Goodbye!");
    anvil_build_ret(ctx, str2);
    
    /* Generate code */
    char *output = NULL;
    size_t len = 0;
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    
    if (err == ANVIL_OK && output) {
        printf("%s\n", output);
        free(output);
    } else {
        fprintf(stderr, "Code generation failed\n");
    }
    
    anvil_module_destroy(mod);
}

int main(int argc, char **argv)
{
    anvil_ctx_t *ctx;
    arch_config_t config;
    
    EXAMPLE_SETUP(argc, argv, ctx, config, "ANVIL String Test");
    
    test_strings(ctx);
    
    anvil_ctx_destroy(ctx);
    return 0;
}
