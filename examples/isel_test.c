#include <anvil.h>
#include <stdio.h>

int main(void) {
    anvil_init();
    
    AnvilModule* mod = anvil_module_new("isel_test");
    
    AnvilFunc* fn = anvil_func_new(mod, "mul_by_8", anvil_type_i32());
    AnvilVar* x = anvil_func_add_param(fn, "x", anvil_type_i32());
    
    AnvilValue* val = anvil_load(fn, x);
    AnvilValue* result = anvil_mul(fn, val, anvil_const_i32(fn, 8));
    anvil_ret(fn, result);
    
    AnvilFunc* fn2 = anvil_func_new(mod, "div_by_4", anvil_type_i32());
    AnvilVar* y = anvil_func_add_param(fn2, "y", anvil_type_i32());
    
    AnvilValue* val2 = anvil_load(fn2, y);
    AnvilValue* result2 = anvil_div(fn2, val2, anvil_const_i32(fn2, 4));
    anvil_ret(fn2, result2);
    
    AnvilFunc* fn3 = anvil_func_new(mod, "mod_by_16", anvil_type_i32());
    AnvilVar* z = anvil_func_add_param(fn3, "z", anvil_type_i32());
    
    AnvilValue* val3 = anvil_load(fn3, z);
    AnvilValue* result3 = anvil_mod(fn3, val3, anvil_const_i32(fn3, 16));
    anvil_ret(fn3, result3);
    
    AnvilFunc* fn4 = anvil_func_new(mod, "mul_by_3", anvil_type_i32());
    AnvilVar* w = anvil_func_add_param(fn4, "w", anvil_type_i32());
    
    AnvilValue* val4 = anvil_load(fn4, w);
    AnvilValue* result4 = anvil_mul(fn4, val4, anvil_const_i32(fn4, 3));
    anvil_ret(fn4, result4);
    
    printf("=== Instruction Selection Test ===\n\n");
    
    printf("--- x86_64 Linux ---\n");
    printf("mul_by_8(x) should use: shl (shift left by 3)\n");
    printf("div_by_4(y) should use: shr (shift right by 2)\n");
    printf("mod_by_16(z) should use: and (mask with 15)\n");
    printf("mul_by_3(w) should use: lea (on x86_64)\n\n");
    
    AnvilTarget target = anvil_target_x86_64_linux();
    AnvilCompileResult result_x86 = anvil_compile(mod, target, ANVIL_OPT_STANDARD);
    
    if (result_x86.errors) {
        fprintf(stderr, "Error: %s\n", result_x86.errors);
    } else {
        printf("%s\n", result_x86.code);
    }
    anvil_result_free(&result_x86);
    
    printf("--- ARM64 Linux ---\n");
    printf("mul_by_8(x) should use: lsl (shift left by 3)\n");
    printf("div_by_4(y) should use: lsr (shift right by 2)\n");
    printf("mod_by_16(z) should use: and (mask with 15)\n\n");
    
    AnvilTarget target_arm = anvil_target_arm64_linux();
    AnvilCompileResult result_arm = anvil_compile(mod, target_arm, ANVIL_OPT_STANDARD);
    
    if (result_arm.errors) {
        fprintf(stderr, "Error: %s\n", result_arm.errors);
    } else {
        printf("%s\n", result_arm.code);
    }
    anvil_result_free(&result_arm);
    
    printf("--- PPC64 Linux ---\n");
    printf("mul_by_8(x) should use: sldi (shift left by 3)\n");
    printf("div_by_4(y) should use: srdi (shift right by 2)\n");
    printf("mod_by_16(z) should use: andi (mask with 15)\n\n");
    
    AnvilTarget target_ppc = anvil_target_ppc64_linux();
    AnvilCompileResult result_ppc = anvil_compile(mod, target_ppc, ANVIL_OPT_STANDARD);
    
    if (result_ppc.errors) {
        fprintf(stderr, "Error: %s\n", result_ppc.errors);
    } else {
        printf("%s\n", result_ppc.code);
    }
    anvil_result_free(&result_ppc);
    
    anvil_module_free(mod);
    anvil_shutdown();
    
    return 0;
}
