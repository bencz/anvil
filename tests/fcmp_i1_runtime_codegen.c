#include <anvil/anvil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *message)
{
    fprintf(stderr, "FCMP/i1 runtime generator: %s\n", message);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "win64") != 0))
        return fail("expected an output assembly path and optional 'win64' target");
    anvil_ctx_t *ctx = anvil_ctx_create();
    if (!ctx || anvil_ctx_set_target(ctx, ANVIL_ARCH_X86_64) != ANVIL_OK)
        return fail("could not create native x86-64 context");
    if (argc == 3 && anvil_ctx_set_abi(ctx, ANVIL_ABI_WIN64) != ANVIL_OK)
        return fail("could not select Win64 ABI");

    anvil_module_t *mod = anvil_module_create(ctx, "fcmp_i1_runtime");
    if (!mod) return fail("could not create module");

    anvil_type_t *i1 = anvil_type_i1(ctx);
    anvil_type_t *u8 = anvil_type_u8(ctx);
    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_type_t *pf1 = anvil_type_ptr(ctx, i1);
    anvil_type_t *fp_params[] = { f64, f64 };
    for (int pred = ANVIL_FCMP_FALSE; pred <= ANVIL_FCMP_TRUE; pred++) {
        char name[32];
        snprintf(name, sizeof(name), "anvil_fcmp_%d", pred);
        anvil_func_t *fn = anvil_func_create(mod, name,
            anvil_type_func(ctx, i1, fp_params, 2, false), ANVIL_LINK_EXTERNAL);
        if (!fn) return fail("could not create FCMP function");
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *v = anvil_build_fcmp(ctx, (anvil_fcmp_pred_t)pred,
            anvil_func_get_param(fn, 0), anvil_func_get_param(fn, 1), "cmp");
        if (!v || !anvil_build_ret(ctx, v)) return fail("could not build FCMP body");
    }

    anvil_type_t *ptr_params[] = { pf1 };
    anvil_func_t *load = anvil_func_create(mod, "anvil_load_i1",
        anvil_type_func(ctx, i1, ptr_params, 1, false), ANVIL_LINK_EXTERNAL);
    if (!load) return fail("could not create i1 load function");
    anvil_set_insert_point(ctx, anvil_func_get_entry(load));
    anvil_value_t *loaded = anvil_build_load(ctx, i1,
        anvil_func_get_param(load, 0), "loaded");
    if (!loaded || !anvil_build_ret(ctx, loaded)) return fail("could not build i1 load");

    anvil_type_t *trunc_params[] = { u8 };
    anvil_func_t *trunc = anvil_func_create(mod, "anvil_trunc_i1",
        anvil_type_func(ctx, i1, trunc_params, 1, false), ANVIL_LINK_EXTERNAL);
    if (!trunc) return fail("could not create i1 trunc function");
    anvil_set_insert_point(ctx, anvil_func_get_entry(trunc));
    anvil_value_t *truncated = anvil_build_trunc(ctx,
        anvil_func_get_param(trunc, 0), i1, "truncated");
    if (!truncated || !anvil_build_ret(ctx, truncated)) return fail("could not build i1 trunc");

    anvil_type_t *store_params[] = { i1, pf1 };
    anvil_func_t *store = anvil_func_create(mod, "anvil_store_i1",
        anvil_type_func(ctx, anvil_type_void(ctx), store_params, 2, false),
        ANVIL_LINK_EXTERNAL);
    if (!store) return fail("could not create i1 store function");
    anvil_set_insert_point(ctx, anvil_func_get_entry(store));
    if (!anvil_build_store(ctx, anvil_func_get_param(store, 0),
                           anvil_func_get_param(store, 1)) ||
        !anvil_build_ret_void(ctx)) return fail("could not build i1 store");

    anvil_type_t *one_fp[] = { f64 };
    anvil_func_t *fp_to_i1 = anvil_func_create(mod, "anvil_fptoui_i1",
        anvil_type_func(ctx, i1, one_fp, 1, false), ANVIL_LINK_EXTERNAL);
    if (!fp_to_i1) return fail("could not create FPTOUI i1 function");
    anvil_set_insert_point(ctx, anvil_func_get_entry(fp_to_i1));
    anvil_value_t *fp_bool = anvil_build_fptoui(ctx,
        anvil_func_get_param(fp_to_i1, 0), i1, "fp_bool");
    if (!fp_bool || !anvil_build_ret(ctx, fp_bool)) return fail("could not build FPTOUI i1");

    char *assembly = NULL;
    size_t assembly_len = 0;
    if (anvil_module_codegen(mod, &assembly, &assembly_len) != ANVIL_OK ||
        !assembly) return fail("native module code generation failed");
    FILE *out = fopen(argv[1], "wb");
    if (!out || fwrite(assembly, 1, assembly_len, out) != assembly_len ||
        fclose(out) != 0) return fail("could not write assembly");
    free(assembly);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    return 0;
}
