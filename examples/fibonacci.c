#include <anvil.h>
#include <stdio.h>

int main(void) {
    anvil_init();
    
    AnvilModule* mod = anvil_module_new("fibonacci");
    
    AnvilFunc* fn = anvil_func_new(mod, "fib", anvil_type_i32());
    AnvilVar* n = anvil_func_add_param(fn, "n", anvil_type_i32());
    
    AnvilValue* n_val = anvil_load(fn, n);
    AnvilValue* two = anvil_const_i32(fn, 2);
    AnvilValue* cond = anvil_lt(fn, n_val, two);
    
    AnvilIf* if_stmt = anvil_if_begin(fn, cond);
    {
        anvil_ret(fn, anvil_load(fn, n));
    }
    anvil_if_else(if_stmt);
    {
        AnvilValue* n_minus_1 = anvil_sub(fn, anvil_load(fn, n), anvil_const_i32(fn, 1));
        AnvilValue* n_minus_2 = anvil_sub(fn, anvil_load(fn, n), anvil_const_i32(fn, 2));
        
        AnvilValue* fib_n1 = anvil_call(fn, "fib", anvil_type_i32(), 1, n_minus_1);
        AnvilValue* fib_n2 = anvil_call(fn, "fib", anvil_type_i32(), 1, n_minus_2);
        
        AnvilValue* result = anvil_add(fn, fib_n1, fib_n2);
        anvil_ret(fn, result);
    }
    anvil_if_end(if_stmt);
    
    AnvilTarget target = anvil_target_x86_64_linux();
    AnvilCompileResult result = anvil_compile(mod, target, ANVIL_OPT_STANDARD);
    
    if (result.errors) {
        fprintf(stderr, "Compilation failed:\n%s\n", result.errors);
    } else {
        printf("Generated assembly for fibonacci:\n%s", result.code);
    }
    
    anvil_result_free(&result);
    anvil_module_free(mod);
    anvil_shutdown();
    
    return 0;
}
