#include <anvil.h>
#include <stdio.h>

int main(void) {
    anvil_init();
    
    AnvilModule* mod = anvil_module_new("sum_array");
    
    AnvilFunc* fn = anvil_func_new(mod, "sum_array", anvil_type_i32());
    AnvilVar* arr = anvil_func_add_param(fn, "arr", anvil_type_ptr(mod, anvil_type_i32()));
    AnvilVar* len = anvil_func_add_param(fn, "len", anvil_type_i32());
    
    AnvilVar* sum = anvil_func_add_local(fn, "sum", anvil_type_i32());
    AnvilVar* i = anvil_func_add_local(fn, "i", anvil_type_i32());
    
    anvil_store(fn, sum, anvil_const_i32(fn, 0));
    
    AnvilLoop* loop = anvil_for_begin(fn, i, 
                                       anvil_const_i32(fn, 0), 
                                       anvil_load(fn, len),
                                       anvil_const_i32(fn, 1));
    {
        AnvilValue* elem = anvil_deref(fn, 
            anvil_index(fn, anvil_load(fn, arr), anvil_load(fn, i)));
        anvil_store(fn, sum, 
            anvil_add(fn, anvil_load(fn, sum), elem));
    }
    anvil_for_end(loop);
    
    anvil_ret(fn, anvil_load(fn, sum));
    
    AnvilTarget target = anvil_target_x86_64_linux();
    AnvilCompileResult result = anvil_compile(mod, target, ANVIL_OPT_STANDARD);
    
    if (result.errors) {
        fprintf(stderr, "Compilation failed:\n%s\n", result.errors);
    } else {
        printf("Generated assembly for sum_array:\n%s", result.code);
    }
    
    anvil_result_free(&result);
    anvil_module_free(mod);
    anvil_shutdown();
    
    return 0;
}
