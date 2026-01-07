#include <anvil.h>
#include <stdio.h>

/*
 * Example: printf with 31 arguments
 * Demonstrates variadic function calls with many arguments.
 * On ARM64 Apple, variadic args go on the stack.
 * On x86_64 System V, first 6 int args in registers, rest on stack.
 */

int main(void) {
    anvil_init();
    
    AnvilModule* mod = anvil_module_new("printf_many_args");
    
    AnvilFunc* main_fn = anvil_func_new(mod, "main", anvil_type_i32());
    
    /* Create 30 integer values to print */
    AnvilValue* args[31];
    args[0] = anvil_const_string(mod, 
        "Values: %d %d %d %d %d %d %d %d %d %d "
        "%d %d %d %d %d %d %d %d %d %d "
        "%d %d %d %d %d %d %d %d %d %d\n");
    
    for (int i = 1; i <= 30; i++) {
        args[i] = anvil_const_i32(main_fn, i);
    }
    
    /* Call printf with 31 args: 1 format string + 30 integers */
    /* num_fixed_args = 1 (format string), num_args = 31 */
    AnvilValue** arg_ptrs[31];
    for (int i = 0; i < 31; i++) {
        arg_ptrs[i] = &args[i];
    }
    
    /* We need to use the builder directly for many args */
    AnvilValue* arg_array[31];
    arg_array[0] = anvil_const_string(mod, 
        "Values: %d %d %d %d %d %d %d %d %d %d "
        "%d %d %d %d %d %d %d %d %d %d "
        "%d %d %d %d %d %d %d %d %d %d\n");
    for (int i = 1; i <= 30; i++) {
        arg_array[i] = anvil_const_i32(main_fn, i);
    }
    
    /* Use anvil_call_variadic: 1 fixed arg, 31 total args */
    anvil_call_variadic(main_fn, "printf", anvil_type_i32(), 1, 31,
        arg_array[0], arg_array[1], arg_array[2], arg_array[3], arg_array[4],
        arg_array[5], arg_array[6], arg_array[7], arg_array[8], arg_array[9],
        arg_array[10], arg_array[11], arg_array[12], arg_array[13], arg_array[14],
        arg_array[15], arg_array[16], arg_array[17], arg_array[18], arg_array[19],
        arg_array[20], arg_array[21], arg_array[22], arg_array[23], arg_array[24],
        arg_array[25], arg_array[26], arg_array[27], arg_array[28], arg_array[29],
        arg_array[30]);
    
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
