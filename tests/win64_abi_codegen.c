#include <anvil/anvil.h>

#include <stdio.h>
#include <stdlib.h>

static int fail(const char *message)
{
    fprintf(stderr, "win64 ABI generator: %s\n", message);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) return fail("expected an output assembly path");
    anvil_ctx_t *ctx = anvil_ctx_create();
    if (!ctx || anvil_ctx_set_target(ctx, ANVIL_ARCH_X86_64) != ANVIL_OK ||
        anvil_ctx_set_abi(ctx, ANVIL_ABI_WIN64) != ANVIL_OK)
        return fail("could not create a Win64 x86-64 context");
    anvil_module_t *mod = anvil_module_create(ctx, "win64_conformance");
    if (!mod) return fail("could not create module");

    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_type_t *sum_params[] = { i64, i64, i64, i64, i64 };
    anvil_type_t *sum_type = anvil_type_func(ctx, i64, sum_params, 5, false);
    anvil_func_t *sum = anvil_func_create(mod, "anvil_sum5", sum_type,
                                          ANVIL_LINK_EXTERNAL);
    if (!sum) return fail("could not create integer callee");
    anvil_set_insert_point(ctx, anvil_func_get_entry(sum));
    anvil_value_t *acc = anvil_func_get_param(sum, 0);
    for (size_t i = 1; i < 5; i++)
        acc = anvil_build_add(ctx, acc, anvil_func_get_param(sum, i), "sum");
    if (!acc || !anvil_build_ret(ctx, acc)) return fail("could not build integer callee");

    anvil_type_t *fp_params[] = { f64, f64, f64, f64 };
    anvil_type_t *fp_type = anvil_type_func(ctx, f64, fp_params, 4, false);
    anvil_func_t *fp = anvil_func_create(mod, "anvil_fp_pressure", fp_type,
                                         ANVIL_LINK_EXTERNAL);
    if (!fp) return fail("could not create FP callee");
    anvil_set_insert_point(ctx, anvil_func_get_entry(fp));
    acc = anvil_build_fadd(ctx, anvil_func_get_param(fp, 0),
                           anvil_func_get_param(fp, 1), "fp01");
    acc = anvil_build_fadd(ctx, acc, anvil_func_get_param(fp, 2), "fp012");
    acc = anvil_build_fadd(ctx, acc, anvil_func_get_param(fp, 3), "fp0123");
    if (!acc || !anvil_build_ret(ctx, acc)) return fail("could not build FP callee");

    anvil_type_t *probe_fixed[] = { i64 };
    anvil_type_t *probe_type = anvil_type_func(ctx, i64, probe_fixed, 1, true);
    anvil_func_t *probe = anvil_func_declare(mod, "win_probe", probe_type);
    anvil_type_t *caller_type = anvil_type_func(ctx, i64, NULL, 0, false);
    anvil_func_t *caller = anvil_func_create(mod, "anvil_call_variadic",
                                             caller_type, ANVIL_LINK_EXTERNAL);
    if (!probe || !caller) return fail("could not create variadic caller");
    anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
    anvil_value_t *args[] = {
        anvil_const_i64(ctx, 42), anvil_const_f64(ctx, 1.25),
        anvil_const_f64(ctx, -2.5), anvil_const_f64(ctx, 3.75),
        anvil_const_i64(ctx, 99)
    };
    anvil_value_t *result = NULL;
    if (!anvil_build_call_checked(ctx, anvil_func_get_value(probe), args, 5,
                                  "probe_result", &result) ||
        !result || !anvil_build_ret(ctx, result))
        return fail("could not build variadic caller");

    char *assembly = NULL;
    size_t assembly_len = 0;
    if (anvil_module_codegen(mod, &assembly, &assembly_len) != ANVIL_OK ||
        !assembly)
        return fail("Win64 module code generation failed");
    FILE *out = fopen(argv[1], "wb");
    if (!out || fwrite(assembly, 1, assembly_len, out) != assembly_len ||
        fclose(out) != 0) {
        free(assembly);
        return fail("could not write generated assembly");
    }
    free(assembly);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    return 0;
}
