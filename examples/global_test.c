/*
 * ANVIL - Global Variables Test
 * 
 * Tests global variable support on all backends
 * 
 * Usage: global_test [arch]
 *   arch: x86, x86_64, s370, s370_xa, s390, zarch, ppc32, ppc64, ppc64le, arm64
 */

#include "anvil/anvil.h"
#include <stdio.h>
#include <stdlib.h>
#include "arch_select.h"

/*
 * Generate a simple program that uses global variables:
 * 
 * int counter = 0;
 * int increment() {
 *     counter = counter + 1;
 *     return counter;
 * }
 */
static void test_global_counter(anvil_ctx_t *ctx)
{
    printf("\n=== Testing global variables ===\n");
    
    anvil_module_t *mod = anvil_module_create(ctx, "global_test");
    
    /* Create global variable: int counter */
    anvil_value_t *counter = anvil_module_add_global(mod, "counter", 
        anvil_type_i32(ctx), ANVIL_LINK_INTERNAL);
    
    /* Create function: int increment() */
    anvil_type_t *func_type = anvil_type_func(ctx, anvil_type_i32(ctx), NULL, 0, false);
    anvil_func_t *func = anvil_func_create(mod, "increment", func_type, ANVIL_LINK_EXTERNAL);
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    /* Load counter value */
    anvil_value_t *val = anvil_build_load(ctx, anvil_type_i32(ctx), counter, "val");
    
    /* Add 1 */
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    anvil_value_t *new_val = anvil_build_add(ctx, val, one, "new_val");
    
    /* Store back to counter */
    anvil_build_store(ctx, new_val, counter);
    
    /* Return new value */
    anvil_build_ret(ctx, new_val);
    
    /* Generate code */
    char *output = NULL;
    size_t len = 0;
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    
    if (err == ANVIL_OK && output) {
        printf("%s\n", output);
        free(output);
    } else {
        printf("Error generating code: %s\n", anvil_ctx_get_error(ctx));
    }
    
    anvil_module_destroy(mod);
}

/*
 * Test different global types
 */
static void test_global_types(anvil_ctx_t *ctx)
{
    printf("\n=== Testing global types ===\n");
    
    anvil_module_t *mod = anvil_module_create(ctx, "types_test");
    
    /* Create globals of different types */
    anvil_module_add_global(mod, "g_byte", anvil_type_i8(ctx), ANVIL_LINK_INTERNAL);
    anvil_module_add_global(mod, "g_short", anvil_type_i16(ctx), ANVIL_LINK_INTERNAL);
    anvil_module_add_global(mod, "g_int", anvil_type_i32(ctx), ANVIL_LINK_INTERNAL);
    anvil_module_add_global(mod, "g_long", anvil_type_i64(ctx), ANVIL_LINK_INTERNAL);
    anvil_module_add_global(mod, "g_float", anvil_type_f32(ctx), ANVIL_LINK_INTERNAL);
    anvil_module_add_global(mod, "g_double", anvil_type_f64(ctx), ANVIL_LINK_INTERNAL);
    
    /* Create a dummy function to make the module valid */
    anvil_type_t *func_type = anvil_type_func(ctx, anvil_type_void(ctx), NULL, 0, false);
    anvil_func_t *func = anvil_func_create(mod, "dummy", func_type, ANVIL_LINK_EXTERNAL);
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    anvil_build_ret_void(ctx);
    
    /* Generate code */
    char *output = NULL;
    size_t len = 0;
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    
    if (err == ANVIL_OK && output) {
        printf("%s\n", output);
        free(output);
    } else {
        printf("Error generating code: %s\n", anvil_ctx_get_error(ctx));
    }
    
    anvil_module_destroy(mod);
}

int main(int argc, char **argv)
{
    anvil_ctx_t *ctx;
    arch_config_t config;
    
    EXAMPLE_SETUP(argc, argv, ctx, config, "ANVIL Global Variables Test");
    
    test_global_counter(ctx);
    test_global_types(ctx);
    
    anvil_ctx_destroy(ctx);
    
    printf("\nDone!\n");
    return 0;
}
