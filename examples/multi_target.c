#include <anvil.h>
#include <stdio.h>

int main(void) {
    anvil_init();
    
    AnvilModule* mod = anvil_module_new("multi_target");
    
    AnvilFunc* fn = anvil_func_new(mod, "add_numbers", anvil_type_i32());
    AnvilVar* a = anvil_func_add_param(fn, "a", anvil_type_i32());
    AnvilVar* b = anvil_func_add_param(fn, "b", anvil_type_i32());
    
    AnvilValue* sum = anvil_add(fn, anvil_load(fn, a), anvil_load(fn, b));
    anvil_ret(fn, sum);
    
    printf("=== x86_64 Linux (System V ABI) ===\n");
    {
        AnvilTarget target = anvil_target_x86_64_linux();
        AnvilCompileResult result = anvil_compile(mod, target, ANVIL_OPT_STANDARD);
        if (result.errors) {
            fprintf(stderr, "Error: %s\n", result.errors);
        } else {
            printf("%s\n", result.code);
        }
        anvil_result_free(&result);
    }
    
    printf("=== x86_64 Windows (Win64 ABI) ===\n");
    {
        AnvilTarget target = anvil_target_x86_64_windows();
        AnvilCompileResult result = anvil_compile(mod, target, ANVIL_OPT_STANDARD);
        if (result.errors) {
            fprintf(stderr, "Error: %s\n", result.errors);
        } else {
            printf("%s\n", result.code);
        }
        anvil_result_free(&result);
    }
    
    printf("=== ARM64 Linux (AAPCS64) ===\n");
    {
        AnvilTarget target = anvil_target_arm64_linux();
        AnvilCompileResult result = anvil_compile(mod, target, ANVIL_OPT_STANDARD);
        if (result.errors) {
            fprintf(stderr, "Error: %s\n", result.errors);
        } else {
            printf("%s\n", result.code);
        }
        anvil_result_free(&result);
    }
    
    printf("=== ARM64 macOS (Apple Silicon) ===\n");
    {
        AnvilTarget target = anvil_target_arm64_macos();
        AnvilCompileResult result = anvil_compile(mod, target, ANVIL_OPT_STANDARD);
        if (result.errors) {
            fprintf(stderr, "Error: %s\n", result.errors);
        } else {
            printf("%s\n", result.code);
        }
        anvil_result_free(&result);
    }

    printf("=== PPC64 Big Endian Assembly ABI elfV2 ===\n");
    {
        AnvilTarget target = anvil_target_ppc64_linux();
        AnvilCompileResult result = anvil_compile(mod, target, ANVIL_OPT_STANDARD);
        if (result.errors) {
            fprintf(stderr, "Error: %s\n", result.errors);
        } else {
            printf("%s\n", result.code);
        }
        anvil_result_free(&result);
    }
    
    anvil_module_free(mod);
    anvil_shutdown();
    
    return 0;
}
