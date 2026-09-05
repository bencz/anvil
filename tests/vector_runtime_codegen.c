#include <anvil/anvil_internal.h>
#include <anvil/anvil_opt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;

    bool windows = strcmp(argv[2], "win64") == 0;
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_ctx_set_abi(ctx, windows ? ANVIL_ABI_WIN64 : ANVIL_ABI_SYSV);
    anvil_module_t *module = anvil_module_create(ctx, "vector_runtime");
    anvil_type_t *void_type = anvil_type_void(ctx);
    anvil_type_t *clobber_type = anvil_type_func(ctx, void_type, NULL, 0, false);
    anvil_func_t *clobber = anvil_func_declare(module, "vector_clobber", clobber_type);
    anvil_op_t operations[] = { ANVIL_OP_FADD, ANVIL_OP_FSUB, ANVIL_OP_FMUL, ANVIL_OP_FDIV };
    bool valid = true;
    for (unsigned width = 0; width < 2; width++)
    {
        anvil_type_t *element = width ? anvil_type_f64(ctx) : anvil_type_f32(ctx);
        size_t lanes = width ? 2 : 4;
        anvil_type_t *array = anvil_type_array(ctx, element, lanes * 3);
        anvil_value_t *storage = anvil_module_add_global(module, width ? "slp_data64" : "slp_data32", array, ANVIL_LINK_EXTERNAL);
        anvil_type_t *vector = anvil_type_vector(ctx, element, width ? 2 : 4);
        anvil_type_t *pointer = anvil_type_ptr(ctx, vector);
        anvil_type_t *parameters[] = { pointer, pointer, pointer };
        anvil_type_t *type = anvil_type_func(ctx, void_type, parameters, 3, false);
        for (unsigned operation = 0; operation < 4; operation++)
        {
            char name[64];
            snprintf(name, sizeof(name), "vector_%u_%u", width ? 64 : 32, operation);
            anvil_func_t *func = anvil_func_create(module, name, type, ANVIL_LINK_EXTERNAL);
            anvil_set_insert_point(ctx, func->entry);
            anvil_value_t *left = anvil_build_load(ctx, vector, anvil_func_get_param(func, 0), "left");
            anvil_value_t *right = anvil_build_load(ctx, vector, anvil_func_get_param(func, 1), "right");
            anvil_instr_t *binary = anvil_instr_create(ctx, operations[operation], vector, "result");
            anvil_value_t *operands[] = { left, right };
            valid &= binary && anvil_instr_add_operands(binary, operands, 2) && anvil_instr_insert(ctx, binary);
            valid &= anvil_build_call_checked(ctx, anvil_func_get_value(clobber), NULL, 0, NULL, NULL);
            valid &= anvil_build_store(ctx, binary->result, anvil_func_get_param(func, 2));
            valid &= anvil_build_ret_void(ctx);

            snprintf(name, sizeof(name), "slp_%u_%u", width ? 64 : 32, operation);
            anvil_func_t *scalar = anvil_func_create(module, name, clobber_type, ANVIL_LINK_EXTERNAL);
            anvil_func_set_fp_vectorization(scalar, true);
            anvil_set_insert_point(ctx, scalar->entry);
            anvil_type_t *element_pointer = anvil_type_ptr(ctx, element);
            anvil_value_t *base = anvil_build_bitcast(ctx, anvil_const_symbol_addr(storage), element_pointer, "base");
            for (size_t lane = 0; lane < lanes; lane++)
            {
                anvil_value_t *addresses[3];
                for (size_t stream = 0; stream < 3; stream++)
                {
                    anvil_value_t *index = anvil_const_i64(ctx, (int64_t)(lane + stream * lanes));
                    addresses[stream] = anvil_build_gep(ctx, element, base, &index, 1, "address");
                }

                anvil_value_t *a = anvil_build_load(ctx, element, addresses[0], "left");
                anvil_value_t *b = anvil_build_load(ctx, element, addresses[1], "right");
                anvil_instr_t *arithmetic = anvil_instr_create(ctx, operations[operation], element, "result");
                anvil_value_t *values[] = { a, b };
                valid &= arithmetic && anvil_instr_add_operands(arithmetic, values, 2) && anvil_instr_insert(ctx, arithmetic);
                valid &= anvil_build_store(ctx, arithmetic->result, addresses[2]);
            }

            valid &= anvil_build_ret_void(ctx);
        }
    }

    anvil_pass_manager_t *manager = anvil_pass_manager_create(ctx);
    anvil_pass_manager_set_level(manager, atoi(argv[3]) == 3 ? ANVIL_OPT_AGGRESSIVE : ANVIL_OPT_NONE);
    valid &= anvil_pass_manager_run_module(manager, module) != ANVIL_PASS_RUN_ERROR;
    char *assembly = NULL;
    size_t length = 0;
    valid &= anvil_module_codegen(module, &assembly, &length) == ANVIL_OK;
    if (valid)
    {
        FILE *output = fopen(argv[1], "wb");
        valid = output && fwrite(assembly, 1, length, output) == length;
        if (output)
        {
            fprintf(output, "\n.text\n.globl vector_clobber\nvector_clobber:\n");
            for (unsigned reg = 0; reg < (windows ? 6u : 16u); reg++)
                fprintf(output, "\tpxor %%xmm%u, %%xmm%u\n", reg, reg);

            fprintf(output, "\tret\n");
            valid &= fclose(output) == 0;
        }
    }

    if (!valid)
        fprintf(stderr, "vector generation failed: %s\n", anvil_ctx_get_error(ctx));

    free(assembly);
    anvil_pass_manager_destroy(manager);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid ? 0 : 1;
}
