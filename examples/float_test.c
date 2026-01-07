#include <anvil.h>
#include <stdio.h>

int main(void) {
    anvil_init();
    
    AnvilModule* mod = anvil_module_new("float_test");
    
    AnvilFunc* fn_add = anvil_func_new(mod, "float_add", anvil_type_f64());
    AnvilVar* a = anvil_func_add_param(fn_add, "a", anvil_type_f64());
    AnvilVar* b = anvil_func_add_param(fn_add, "b", anvil_type_f64());
    
    AnvilValue* va = anvil_load(fn_add, a);
    AnvilValue* vb = anvil_load(fn_add, b);
    AnvilValue* sum = anvil_fadd(fn_add, va, vb);
    anvil_ret(fn_add, sum);
    
    AnvilFunc* fn_mul = anvil_func_new(mod, "float_mul", anvil_type_f64());
    AnvilVar* x = anvil_func_add_param(fn_mul, "x", anvil_type_f64());
    AnvilVar* y = anvil_func_add_param(fn_mul, "y", anvil_type_f64());
    
    AnvilValue* vx = anvil_load(fn_mul, x);
    AnvilValue* vy = anvil_load(fn_mul, y);
    AnvilValue* prod = anvil_fmul(fn_mul, vx, vy);
    anvil_ret(fn_mul, prod);
    
    AnvilFunc* fn_dot = anvil_func_new(mod, "dot_product_4", anvil_type_f64());
    AnvilVar* v1_0 = anvil_func_add_param(fn_dot, "v1_0", anvil_type_f64());
    AnvilVar* v1_1 = anvil_func_add_param(fn_dot, "v1_1", anvil_type_f64());
    AnvilVar* v1_2 = anvil_func_add_param(fn_dot, "v1_2", anvil_type_f64());
    AnvilVar* v1_3 = anvil_func_add_param(fn_dot, "v1_3", anvil_type_f64());
    AnvilVar* v2_0 = anvil_func_add_param(fn_dot, "v2_0", anvil_type_f64());
    AnvilVar* v2_1 = anvil_func_add_param(fn_dot, "v2_1", anvil_type_f64());
    AnvilVar* v2_2 = anvil_func_add_param(fn_dot, "v2_2", anvil_type_f64());
    AnvilVar* v2_3 = anvil_func_add_param(fn_dot, "v2_3", anvil_type_f64());
    
    AnvilValue* m0 = anvil_fmul(fn_dot, anvil_load(fn_dot, v1_0), anvil_load(fn_dot, v2_0));
    AnvilValue* m1 = anvil_fmul(fn_dot, anvil_load(fn_dot, v1_1), anvil_load(fn_dot, v2_1));
    AnvilValue* m2 = anvil_fmul(fn_dot, anvil_load(fn_dot, v1_2), anvil_load(fn_dot, v2_2));
    AnvilValue* m3 = anvil_fmul(fn_dot, anvil_load(fn_dot, v1_3), anvil_load(fn_dot, v2_3));
    
    AnvilValue* s01 = anvil_fadd(fn_dot, m0, m1);
    AnvilValue* s23 = anvil_fadd(fn_dot, m2, m3);
    AnvilValue* dot = anvil_fadd(fn_dot, s01, s23);
    anvil_ret(fn_dot, dot);
    
    printf("=== Floating Point Test ===\n\n");
    
    printf("--- x86_64 Linux ---\n");
    AnvilTarget target_x86 = anvil_target_x86_64_linux();
    AnvilCompileResult result_x86 = anvil_compile(mod, target_x86, ANVIL_OPT_AGGRESSIVE);
    if (result_x86.errors) {
        fprintf(stderr, "Error: %s\n", result_x86.errors);
    } else {
        printf("%s\n", result_x86.code);
    }
    anvil_result_free(&result_x86);
    
    printf("--- ARM64 Linux ---\n");
    AnvilTarget target_arm = anvil_target_arm64_linux();
    AnvilCompileResult result_arm = anvil_compile(mod, target_arm, ANVIL_OPT_AGGRESSIVE);
    if (result_arm.errors) {
        fprintf(stderr, "Error: %s\n", result_arm.errors);
    } else {
        printf("%s\n", result_arm.code);
    }
    anvil_result_free(&result_arm);
    
    printf("--- PPC64 Linux ---\n");
    AnvilTarget target_ppc = anvil_target_ppc64_linux();
    AnvilCompileResult result_ppc = anvil_compile(mod, target_ppc, ANVIL_OPT_AGGRESSIVE);
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
