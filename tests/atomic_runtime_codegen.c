#include <anvil/anvil.h>
#include <anvil/anvil_opt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 5)
        return 1;

    bool arm64 = strcmp(argv[3], "arm64") == 0;
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arm64 ? ANVIL_ARCH_ARM64 : ANVIL_ARCH_X86_64);
    anvil_ctx_set_abi(ctx, strcmp(argv[3], "win64") == 0 ? ANVIL_ABI_WIN64 : ANVIL_ABI_SYSV);
    anvil_module_t *module = anvil_module_create(ctx, "atomic_runtime");
    anvil_type_t *types[] = { anvil_type_u8(ctx), anvil_type_u16(ctx), anvil_type_u32(ctx), anvil_type_u64(ctx) };
    anvil_type_t *u64 = anvil_type_u64(ctx);
    anvil_type_t *parameters[] = { anvil_type_ptr(ctx, anvil_type_u8(ctx)), u64, u64 };
    anvil_type_t *function_type = anvil_type_func(ctx, u64, parameters, 3, false);
    FILE *header = fopen(argv[2], "w");
    if (!header)
        return 1;

    /* Operation: load=0, store=1, six RMW operations=2..7, compare=8. */
    for (unsigned width = 0; width < 4; width++)
    {
        for (unsigned operation = 0; operation < 9; operation++)
        {
            for (unsigned order = 0; order < 5; order++)
            {
                if ((operation == 0 && (order == ANVIL_ORDER_RELEASE || order == ANVIL_ORDER_ACQ_REL)) ||
                    (operation == 1 && (order == ANVIL_ORDER_ACQUIRE || order == ANVIL_ORDER_ACQ_REL)))
                    continue;

                char name[64];
                snprintf(name, sizeof(name), "atomic_case_%u_%u_%u", width, operation, order);
                fprintf(header, "extern uint64_t %s(void *, uint64_t, uint64_t);\n", name);
                anvil_func_t *function = anvil_func_create(module, name, function_type, ANVIL_LINK_EXTERNAL);
                anvil_set_insert_point(ctx, anvil_func_get_entry(function));
                anvil_value_t *pointer = anvil_build_bitcast(ctx, anvil_func_get_param(function, 0), anvil_type_ptr(ctx, types[width]), "pointer");
                anvil_value_t *value = anvil_func_get_param(function, 1);
                anvil_value_t *expected = anvil_func_get_param(function, 2);
                if (width != 3)
                {
                    value = anvil_build_trunc(ctx, value, types[width], "value");
                    expected = anvil_build_trunc(ctx, expected, types[width], "expected");
                }

                anvil_value_t *result = NULL;
                if (operation == 0)
                    result = anvil_build_atomic_load(ctx, pointer, (anvil_memory_order_t)order, "old");
                else if (operation == 1)
                {
                    anvil_build_atomic_store(ctx, value, pointer, (anvil_memory_order_t)order);
                    result = anvil_const_u64(ctx, 0);
                }
                else if (operation == 8)
                    result = anvil_build_atomic_cmpxchg(ctx, pointer, expected, value, (anvil_memory_order_t)order, ANVIL_ORDER_RELAXED, "old");
                else
                    result = anvil_build_atomic_rmw(ctx, (anvil_atomic_rmw_t)(operation - 2), pointer, value, (anvil_memory_order_t)order, "old");

                if (!result)
                    return 1;

                if (width != 3 && operation != 1)
                    result = anvil_build_zext(ctx, result, u64, "extended");

                anvil_build_ret(ctx, result);
            }
        }
    }

    /* Keep enough independent values live across CAS to force operand/result
     * spills and exercise the atomic scratch-register and splitting contracts. */
    anvil_type_t *wide_pointer = anvil_type_ptr(ctx, u64);
    anvil_type_t *pressure_parameters[] = { wide_pointer, wide_pointer };
    anvil_func_t *pressure = anvil_func_create(module, "atomic_pressure", anvil_type_func(ctx, u64, pressure_parameters, 2, false), ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(pressure));
    anvil_value_t *live[32];
    for (size_t index = 0; index < 32; index++)
    {
        anvil_value_t *offset = anvil_const_u64(ctx, index);
        anvil_value_t *address = anvil_build_gep(ctx, u64, anvil_func_get_param(pressure, 1), &offset, 1, "input.address");
        live[index] = anvil_build_load(ctx, u64, address, "input");
    }

    anvil_value_t *sum = anvil_build_atomic_cmpxchg(ctx, anvil_func_get_param(pressure, 0), live[0], live[1],
                                                  ANVIL_ORDER_SEQ_CST, ANVIL_ORDER_ACQUIRE, "old");
    for (size_t index = 0; index < 32; index++)
        sum = anvil_build_add(ctx, sum, live[index], "sum");

    anvil_build_ret(ctx, sum);
    fprintf(header, "extern uint64_t atomic_pressure(uint64_t *, const uint64_t *);\n");
    fprintf(header, "static const atomic_case cases[] = {\n");
    for (unsigned width = 0; width < 4; width++)
    {
        for (unsigned operation = 0; operation < 9; operation++)
        {
            for (unsigned order = 0; order < 5; order++)
            {
                if ((operation == 0 && (order == ANVIL_ORDER_RELEASE || order == ANVIL_ORDER_ACQ_REL)) ||
                    (operation == 1 && (order == ANVIL_ORDER_ACQUIRE || order == ANVIL_ORDER_ACQ_REL)))
                    continue;

                fprintf(header, "    { atomic_case_%u_%u_%u, %u, %u, %u },\n", width, operation, order, 1u << width, operation, order);
            }
        }
    }
    fprintf(header, "};\n");
    fclose(header);

    anvil_pass_manager_t *manager = anvil_pass_manager_create(ctx);
    anvil_pass_manager_set_level(manager, atoi(argv[4]) == 3 ? ANVIL_OPT_AGGRESSIVE : ANVIL_OPT_NONE);
    bool ok = anvil_pass_manager_run_module(manager, module) != ANVIL_PASS_RUN_ERROR;
    char *assembly = NULL;
    size_t length = 0;
    ok &= anvil_module_codegen(module, &assembly, &length) == ANVIL_OK;
    FILE *output = ok ? fopen(argv[1], "w") : NULL;
    if (output)
    {
        ok &= fwrite(assembly, 1, length, output) == length;
        ok &= fclose(output) == 0;
    }
    else
        ok = false;

    if (!ok)
        fprintf(stderr, "atomic generator failed: %s\n", anvil_ctx_get_error(ctx));

    free(assembly);
    anvil_pass_manager_destroy(manager);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return ok ? 0 : 1;
}
