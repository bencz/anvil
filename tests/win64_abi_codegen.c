#include <anvil/anvil.h>
#include <anvil/anvil_opt.h>

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

    /* Keep a local buffer live across a call with an outgoing stack argument.
     * Dynamic allocation must leave shadow/argument space below the buffer. */
    for (int dynamic = 0; dynamic <= 1; dynamic++)
    {
        anvil_type_t *buffer_type = anvil_type_array(ctx, i64, 16384);
        anvil_type_t *params[] = { i64 };
        anvil_type_t *type = anvil_type_func(ctx, i64, params, 1, false);
        const char *name = dynamic ? "anvil_dynamic_buffer" : "anvil_large_buffer";
        anvil_func_t *fn = anvil_func_create(mod, name, type, ANVIL_LINK_EXTERNAL);
        if (!fn)
            return fail("buffer function");

        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *buffer = dynamic ? anvil_build_alloca_dyn(ctx, i64, anvil_func_get_param(fn, 0), "buffer")
                                        : anvil_build_alloca(ctx, buffer_type, "buffer");
        anvil_value_t *indices[] = { anvil_const_i64(ctx, 0), anvil_const_i64(ctx, 4) };
        anvil_value_t *slot = anvil_build_gep(ctx, dynamic ? i64 : buffer_type, buffer,
                                             dynamic ? indices + 1 : indices, dynamic ? 1 : 2, "slot");
        if (!slot || !anvil_build_store(ctx, anvil_const_i64(ctx, 123), slot))
            return fail("buffer store");

        anvil_value_t *call_args[] = {
            anvil_const_i64(ctx, 1),
            anvil_const_i64(ctx, 2),
            anvil_const_i64(ctx, 3),
            anvil_const_i64(ctx, 4),
            anvil_const_i64(ctx, 999),
        };
        if (!anvil_build_call_checked(ctx, anvil_func_get_value(sum), call_args, 5, "sum", &result))
            return fail("buffer call");

        if (!anvil_build_ret(ctx, anvil_build_load(ctx, i64, slot, "reload")))
            return fail("buffer return");
    }

    anvil_type_t *mixed_params[] = { i64, f64, i64, f64, f64, i64, f64 };
    anvil_type_t *mixed_type = anvil_type_func(ctx, f64, mixed_params, 7, false);
    anvil_func_t *mixed = anvil_func_create(mod, "anvil_mixed", mixed_type, ANVIL_LINK_EXTERNAL);
    if (!mixed)
        return fail("mixed function");

    anvil_set_insert_point(ctx, anvil_func_get_entry(mixed));
    acc = anvil_build_sitofp(ctx, anvil_func_get_param(mixed, 0), f64, "first");
    for (size_t i = 1; i < 7; i++)
    {
        anvil_value_t *arg = anvil_func_get_param(mixed, i);
        if (i == 2 || i == 5)
            arg = anvil_build_sitofp(ctx, arg, f64, "convert");

        acc = anvil_build_fadd(ctx, acc, arg, "mixed_sum");
    }
    if (!acc || !anvil_build_ret(ctx, acc))
        return fail("mixed return");

    anvil_type_t *callback_type = anvil_type_func(ctx, i64, NULL, 0, false);
    anvil_type_t *unwind_params[] = { i64, anvil_type_ptr(ctx, callback_type) };
    anvil_type_t *unwind_type = anvil_type_func(ctx, i64, unwind_params, 2, false);
    anvil_func_t *unwind = anvil_func_create(mod, "anvil_unwind", unwind_type, ANVIL_LINK_EXTERNAL);
    if (!unwind)
        return fail("unwind function");

    anvil_set_insert_point(ctx, anvil_func_get_entry(unwind));
    anvil_value_t *space = anvil_build_alloca_dyn(ctx, i64, anvil_func_get_param(unwind, 0), "space");
    if (!space || !anvil_build_store(ctx, anvil_const_i64(ctx, 42), space) ||
        !anvil_build_call_checked(ctx, anvil_func_get_param(unwind, 1), NULL, 0, "callback", &result) || !result ||
        !anvil_build_ret(ctx, result))
        return fail("unwind callback");

    /* Both operations need distinct literal labels in each generated function. */
    for (int index = 0; index < 2; index++)
    {
        anvil_type_t *params[] = { f64 };
        anvil_type_t *type = anvil_type_func(ctx, f64, params, 1, false);
        anvil_func_t *fn = anvil_func_create(mod, index ? "anvil_negative_abs_b" : "anvil_negative_abs_a", type, ANVIL_LINK_EXTERNAL);
        if (!fn)
            return fail("floating unary function");

        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *absolute = anvil_build_fabs(ctx, anvil_func_get_param(fn, 0), "absolute");
        anvil_value_t *negative = anvil_build_fneg(ctx, absolute, "negative");
        if (!negative || !anvil_build_ret(ctx, negative))
            return fail("floating unary return");
    }

    anvil_type_t *trap_params[] = { anvil_type_ptr(ctx, i64), i64 };
    anvil_type_t *trap_type = anvil_type_func(ctx, i64, trap_params, 2, false);
    anvil_func_t *trap = anvil_func_create(mod, "anvil_store_before_trap", trap_type, ANVIL_LINK_EXTERNAL);
    if (!trap)
        return fail("store-before-trap function");

    anvil_set_insert_point(ctx, anvil_func_get_entry(trap));
    anvil_value_t *observed = anvil_func_get_param(trap, 0);
    if (!anvil_build_store(ctx, anvil_const_i64(ctx, 1), observed))
        return fail("store before division");

    anvil_value_t *quotient = anvil_build_sdiv(ctx, anvil_const_i64(ctx, 42), anvil_func_get_param(trap, 1), "quotient");
    if (!quotient || !anvil_build_store(ctx, anvil_const_i64(ctx, 2), observed) || !anvil_build_ret(ctx, quotient))
        return fail("potentially trapping division");

    anvil_pass_manager_t *optimizer = anvil_pass_manager_create(ctx);
    if (!optimizer || anvil_pass_manager_set_level(optimizer, ANVIL_OPT_AGGRESSIVE) != ANVIL_OK ||
        anvil_pass_manager_run_func(optimizer, trap) == ANVIL_PASS_RUN_ERROR)
        return fail("optimize exception-observable stores");

    anvil_pass_manager_destroy(optimizer);
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
