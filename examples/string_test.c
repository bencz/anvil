/*
 * ANVIL String Support Test
 * Tests string constant generation for all architectures
 */

#include <anvil/anvil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_strings(anvil_arch_t arch, const char *arch_name)
{
    printf("=== Testing strings for %s ===\n\n", arch_name);
    
    anvil_ctx_t *ctx = anvil_ctx_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return;
    }
    
    anvil_ctx_set_target(ctx, arch);
    
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
    anvil_ctx_destroy(ctx);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: %s <arch>\n", argv[0]);
        printf("  arch: s370, s390, zarch, x86, x86_64, ppc32, ppc64, ppc64le\n");
        return 1;
    }
    
    const char *arch_str = argv[1];
    
    if (strcmp(arch_str, "s370") == 0) {
        test_strings(ANVIL_ARCH_S370, "S/370");
    } else if (strcmp(arch_str, "s390") == 0) {
        test_strings(ANVIL_ARCH_S390, "S/390");
    } else if (strcmp(arch_str, "zarch") == 0) {
        test_strings(ANVIL_ARCH_ZARCH, "z/Architecture");
    } else if (strcmp(arch_str, "x86") == 0) {
        test_strings(ANVIL_ARCH_X86, "x86");
    } else if (strcmp(arch_str, "x86_64") == 0) {
        test_strings(ANVIL_ARCH_X86_64, "x86-64");
    } else if (strcmp(arch_str, "ppc32") == 0) {
        test_strings(ANVIL_ARCH_PPC32, "PowerPC 32-bit");
    } else if (strcmp(arch_str, "ppc64") == 0) {
        test_strings(ANVIL_ARCH_PPC64, "PowerPC 64-bit");
    } else if (strcmp(arch_str, "ppc64le") == 0) {
        test_strings(ANVIL_ARCH_PPC64LE, "PowerPC 64-bit LE");
    } else {
        fprintf(stderr, "Unknown architecture: %s\n", arch_str);
        return 1;
    }
    
    return 0;
}
