#include <anvil.h>
#include <stdio.h>

int main(void) {
    anvil_init();
    
    AnvilModule* mod = anvil_module_new("ppc64_test");
    
    AnvilFunc* sum_fn = anvil_func_new(mod, "sum", anvil_type_i64());
    AnvilVar* a = anvil_func_add_param(sum_fn, "a", anvil_type_i64());
    AnvilVar* b = anvil_func_add_param(sum_fn, "b", anvil_type_i64());
    
    AnvilValue* result = anvil_add(sum_fn, anvil_load(sum_fn, a), anvil_load(sum_fn, b));
    anvil_ret(sum_fn, result);
    
    AnvilFunc* main_fn = anvil_func_new(mod, "main", anvil_type_i32());
    
    AnvilValue* x = anvil_const_i64(main_fn, 10);
    AnvilValue* y = anvil_const_i64(main_fn, 20);
    AnvilValue* sum_result = anvil_call(main_fn, "sum", anvil_type_i64(), 2, x, y);
    
    AnvilValue* ret_val = anvil_cast(main_fn, sum_result, anvil_type_i32());
    anvil_ret(main_fn, ret_val);
    
    AnvilTarget target = anvil_target_ppc64_linux();
    
    AnvilCompileResult result_asm = anvil_compile(mod, target, ANVIL_OPT_STANDARD);
    
    if (result_asm.errors) {
        fprintf(stderr, "Compilation failed:\n%s\n", result_asm.errors);
    } else {
        printf("=== PPC64 Big Endian Assembly ===\n");
        printf("%s", result_asm.code);
    }
    
    anvil_result_free(&result_asm);
    anvil_module_free(mod);
    anvil_shutdown();
    
    return 0;
}
