/*
 * ANVIL Hello World Example
 * 
 * Generates assembly code equivalent to:
 *   #include <stdio.h>
 *   int main() {
 *       printf("Hello, World!\n");
 *       return 0;
 *   }
 * 
 * Usage: hello_world [arch]
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
    
    EXAMPLE_SETUP(argc, argv, ctx, config, "ANVIL Hello World Example");
    
    /* Create module */
    anvil_module_t *mod = anvil_module_create(ctx, "hello");
    
    /* Declare printf as external function: int printf(const char *fmt, ...) */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *i8 = anvil_type_i8(ctx);
    anvil_type_t *ptr_i8 = anvil_type_ptr(ctx, i8);
    
    anvil_type_t *printf_params[] = { ptr_i8 };
    anvil_type_t *printf_type = anvil_type_func(ctx, i32, printf_params, 1, true);  /* variadic */
    
    anvil_func_t *printf_func = anvil_func_declare(mod, "printf", printf_type);
    anvil_value_t *printf_val = anvil_func_get_value(printf_func);
    
    /* Create main function: int main(void) */
    anvil_type_t *main_type = anvil_type_func(ctx, i32, NULL, 0, false);
    anvil_func_t *main_func = anvil_func_create(mod, "main", main_type, ANVIL_LINK_EXTERNAL);
    
    /* Get entry block and set insert point */
    anvil_block_t *entry = anvil_func_get_entry(main_func);
    anvil_set_insert_point(ctx, entry);
    
    /* Create string constant "Hello, World!\n" */
    anvil_value_t *hello_str = anvil_const_string(ctx, "Hello, World!\n");
    
    /* Call printf(hello_str) */
    anvil_value_t *args[] = { hello_str };
    anvil_build_call(ctx, printf_type, printf_val, args, 1, "call_printf");
    
    /* Return 0 */
    anvil_value_t *zero = anvil_const_i32(ctx, 0);
    anvil_build_ret(ctx, zero);
    
    /* Generate code */
    char *output = NULL;
    size_t len = 0;
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    
    if (err == ANVIL_OK && output) {
        printf("%s\n", output);
        free(output);
    } else {
        fprintf(stderr, "Code generation failed: %s\n", anvil_ctx_get_error(ctx));
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    
    return 0;
}
