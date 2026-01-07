#include <anvil.h>
#include <stdio.h>

int main(void) {
    anvil_init();
    
    AnvilModule* mod = anvil_module_new("hello");
    
    AnvilFunc* fn = anvil_func_new(mod, "get_answer", anvil_type_i32());
    AnvilValue* val = anvil_const_i32(fn, 42);
    anvil_ret(fn, val);
    
    AnvilTarget target = anvil_target_arm64_macos();
    AnvilCompileResult result = anvil_compile(mod, target, ANVIL_OPT_NONE);
    
    if (result.errors) {
        fprintf(stderr, "Compilation failed:\n%s\n", result.errors);
    } else {
        printf("Generated assembly for ARM64 macOS:\n%s", result.code);
    }
    
    anvil_result_free(&result);
    anvil_module_free(mod);
    anvil_shutdown();
    
    return 0;
}
