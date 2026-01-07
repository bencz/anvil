#include <anvil.h>
#include <stdio.h>

int main(void) {
    anvil_init();
    
    AnvilModule* mod = anvil_module_new("main_example");
    
    AnvilFunc* sum_fn = anvil_func_new(mod, "sum", anvil_type_i32());
    AnvilVar* a = anvil_func_add_param(sum_fn, "a", anvil_type_i32());
    AnvilVar* b = anvil_func_add_param(sum_fn, "b", anvil_type_i32());
    
    AnvilValue* result = anvil_add(sum_fn, anvil_load(sum_fn, a), anvil_load(sum_fn, b));
    anvil_ret(sum_fn, result);
    
    AnvilFunc* main_fn = anvil_func_new(mod, "main", anvil_type_i32());
    
    AnvilValue* x = anvil_const_i32(main_fn, 10);
    AnvilValue* y = anvil_const_i32(main_fn, 32);
    AnvilValue* sum_result = anvil_call(main_fn, "sum", anvil_type_i32(), 2, x, y);
    
    anvil_call_variadic(main_fn, "printf", anvil_type_i32(), 1, 2, 
                         anvil_const_string(mod, "Result: %d\n"), sum_result);
    
    anvil_ret(main_fn, anvil_const_i32(main_fn, 0));
    
    printf("=== ARM64 macOS (Apple Silicon) ===\n");
    {
        AnvilTarget target = anvil_target_arm64_macos();
        AnvilCompileResult result = anvil_compile(mod, target, ANVIL_OPT_NONE);
        if (result.errors) {
            fprintf(stderr, "Error: %s\n", result.errors);
        } else {
            printf("%s\n", result.code);
        }
        anvil_result_free(&result);
    }
    
    printf("=== x86_64 Linux (System V ABI) ===\n");
    {
        AnvilTarget target = anvil_target_x86_64_linux();
        AnvilCompileResult result = anvil_compile(mod, target, ANVIL_OPT_NONE);
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
