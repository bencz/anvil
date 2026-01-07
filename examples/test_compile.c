#include <anvil.h>
#include <stdio.h>

int main(void) {
    anvil_init();
    
    AnvilModule* mod = anvil_module_new("test");
    
    AnvilFunc* sum_fn = anvil_func_new(mod, "sum", anvil_type_i32());
    AnvilVar* a = anvil_func_add_param(sum_fn, "a", anvil_type_i32());
    AnvilVar* b = anvil_func_add_param(sum_fn, "b", anvil_type_i32());
    
    AnvilValue* result = anvil_add(sum_fn, anvil_load(sum_fn, a), anvil_load(sum_fn, b));
    anvil_ret(sum_fn, result);
    
    AnvilTarget target = anvil_target_arm64_macos();
    AnvilCompileResult compile_result = anvil_compile(mod, target, ANVIL_OPT_NONE);
    
    if (compile_result.errors) {
        fprintf(stderr, "Error: %s\n", compile_result.errors);
        return 1;
    }
    
    FILE* f = fopen("/tmp/sum.s", "w");
    if (f) {
        fprintf(f, "%s", compile_result.code);
        fclose(f);
        printf("Assembly written to /tmp/sum.s\n");
        printf("%s", compile_result.code);
    }
    
    anvil_result_free(&compile_result);
    anvil_module_free(mod);
    anvil_shutdown();
    
    return 0;
}
