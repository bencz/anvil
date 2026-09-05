#include <anvil/anvil_analysis.h>
#include <anvil/anvil_opt.h>
#include <stdio.h>
#include <stdlib.h>

static bool run_case(bool multiple_entries, bool invariant, size_t allowance, bool *completed)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "loop_preheader");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *parameters[] = { i32, i32, anvil_type_i1(ctx) };
    anvil_type_t *type = anvil_type_func(ctx, i32, parameters, 3, false);
    anvil_func_t *func = anvil_func_create(module, "iterate", type, ANVIL_LINK_EXTERNAL);
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_block_t *alternate = multiple_entries ? anvil_block_create(func, "alternate") : NULL;
    anvil_block_t *header = anvil_block_create(func, "header");
    anvil_block_t *body = anvil_block_create(func, "body");
    anvil_block_t *exit = anvil_block_create(func, "exit");
    anvil_value_t *zero = anvil_const_i32(ctx, 0);
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    anvil_value_t *two = anvil_const_i32(ctx, 2);

    anvil_set_insert_point(ctx, entry);
    anvil_build_br_cond(ctx, anvil_func_get_param(func, 2), header, alternate ? alternate : exit);
    if (alternate)
    {
        anvil_set_insert_point(ctx, alternate);
        anvil_build_br(ctx, header);
    }

    anvil_set_insert_point(ctx, header);
    anvil_value_t *index = anvil_build_phi(ctx, i32, "index");
    anvil_value_t *sum = anvil_build_phi(ctx, i32, "sum");
    anvil_value_t *condition = anvil_build_cmp_lt(ctx, index, anvil_func_get_param(func, 0), "condition");
    anvil_build_br_cond(ctx, condition, body, exit);

    anvil_set_insert_point(ctx, body);
    anvil_value_t *factor = invariant ? anvil_func_get_param(func, 1) : index;
    anvil_value_t *product = anvil_build_mul(ctx, factor, two, "product");
    anvil_value_t *next_sum = anvil_build_add(ctx, sum, product, "next.sum");
    anvil_value_t *next_index = anvil_build_add(ctx, index, one, "next.index");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(index, zero, entry);
    anvil_phi_add_incoming(sum, one, entry);
    if (alternate)
    {
        anvil_phi_add_incoming(index, one, alternate);
        anvil_phi_add_incoming(sum, two, alternate);
    }
    anvil_phi_add_incoming(index, next_index, body);
    anvil_phi_add_incoming(sum, next_sum, body);

    anvil_set_insert_point(ctx, exit);
    anvil_value_t *result = anvil_build_phi(ctx, i32, "result");
    anvil_phi_add_incoming(result, sum, header);
    if (!alternate)
        anvil_phi_add_incoming(result, zero, entry);

    anvil_build_ret(ctx, result);

    if (allowance != SIZE_MAX)
        anvil_test_fail_alloc_after(ctx, allowance);

    anvil_pass_result_t status = anvil_pass_licm(func);
    anvil_test_disable_alloc_fail(ctx);
    anvil_ctx_clear_error(ctx);
    char error[256] = { 0 };
    bool valid = anvil_func_verify(func, error, sizeof(error));
    *completed = status != ANVIL_PASS_RUN_ERROR;
    if (*completed)
    {
        valid &= status == (invariant ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED);
        if (invariant)
        {
            anvil_block_t *preheader = product->data.instr->parent;
            valid &= preheader != body && preheader != header && preheader->last->true_block == header;
            valid &= index->data.instr->num_phi_incoming == 2 && sum->data.instr->num_phi_incoming == 2;
            if (alternate)
            {
                anvil_instr_t *first = preheader->first;
                anvil_instr_t *second = first->next;
                valid &= first->op == ANVIL_OP_PHI && first->num_phi_incoming == 2;
                valid &= second->op == ANVIL_OP_PHI && second->num_phi_incoming == 2;
                valid &= first->operands[0] == zero && first->operands[1] == one;
                valid &= second->operands[0] == one && second->operands[1] == two;
            }
        }

        valid &= anvil_pass_licm(func) == ANVIL_PASS_RUN_UNCHANGED;
        anvil_pass_manager_t *manager = anvil_pass_manager_create(ctx);
        anvil_pass_manager_set_level(manager, ANVIL_OPT_AGGRESSIVE);
        valid &= anvil_pass_manager_run_func(manager, func) != ANVIL_PASS_RUN_ERROR;
        anvil_pass_manager_destroy(manager);
        char *assembly = NULL;
        size_t length = 0;
        valid &= anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && length != 0;
        free(assembly);
    }

    if (!valid)
        fprintf(stderr, "preheader failure: entries=%u invariant=%u allowance=%zu status=%d error=%s\n", multiple_entries, invariant, allowance, status, error);

    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

int main(void)
{
    for (unsigned multiple = 0; multiple < 2; multiple++)
    {
        bool completed;
        if (!run_case(multiple != 0, false, SIZE_MAX, &completed) || !completed)
            return 1;

        completed = false;
        for (size_t allowance = 0; allowance < 300 && !completed; allowance++)
        {
            if (!run_case(multiple != 0, true, allowance, &completed))
                return 1;
        }

        if (!completed)
            return 1;
    }

    return 0;
}
