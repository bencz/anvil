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
 *   arch: x86, x86_64, s370, s390, zarch, ppc32, ppc64, ppc64le (default: x86_64)
 */

#include <anvil/anvil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    anvil_arch_t arch = ANVIL_ARCH_X86_64;
    const char *arch_name = "x86-64";
    
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
        } else if (strcmp(argv[1], "s390") == 0) {
            arch = ANVIL_ARCH_S390;
            arch_name = "S/390";
        } else if (strcmp(argv[1], "zarch") == 0) {
            arch = ANVIL_ARCH_ZARCH;
            arch_name = "z/Architecture";
        } else if (strcmp(argv[1], "ppc32") == 0) {
            arch = ANVIL_ARCH_PPC32;
            arch_name = "PowerPC 32-bit";
        } else if (strcmp(argv[1], "ppc64") == 0) {
            arch = ANVIL_ARCH_PPC64;
            arch_name = "PowerPC 64-bit";
        } else if (strcmp(argv[1], "ppc64le") == 0) {
            arch = ANVIL_ARCH_PPC64LE;
            arch_name = "PowerPC 64-bit LE";
        } else {
            fprintf(stderr, "Unknown architecture: %s\n", argv[1]);
            fprintf(stderr, "Usage: %s [x86|x86_64|s370|s390|zarch|ppc32|ppc64|ppc64le]\n", argv[0]);
            return 1;
        }
    }
    
    fprintf(stderr, "Generating Hello World for %s...\n", arch_name);
    
    /* Create context */
    anvil_ctx_t *ctx = anvil_ctx_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    anvil_ctx_set_target(ctx, arch);
    
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
