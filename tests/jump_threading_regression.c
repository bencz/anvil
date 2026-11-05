/* Jump threading on an earlier test of the same condition: a loop leaving
 * on c (false) or after c (true) reaches a join that tests c again, holding
 * pure address work its successors read.  The work moves into both
 * successors and each exit edge goes straight to the side c chooses, so the
 * loop no longer keeps c as a value.  Allocation failure at any point leaves
 * valid IR. */
#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include "anvil/anvil_debug.h"
#include <stdio.h>
#include <stdlib.h>

static int failures;

static bool run_case(size_t allowance, bool *completed)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "thread");
    anvil_type_t *i32 = anvil_type_i32(ctx), *p32 = anvil_type_ptr(ctx, i32);
    anvil_type_t *params[] = {p32, i32};
    anvil_func_t *func = anvil_func_create(module, "probe", anvil_type_func(ctx, i32, params, 2, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(func, "header"), *check = anvil_block_create(func, "check");
    anvil_block_t *latch = anvil_block_create(func, "latch"), *join = anvil_block_create(func, "join");
    anvil_block_t *hit = anvil_block_create(func, "hit"), *miss = anvil_block_create(func, "miss");
    anvil_value_t *base = anvil_func_get_param(func, 0), *key = anvil_func_get_param(func, 1);

    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *i = anvil_build_phi(ctx, i32, "i");
    anvil_value_t *slot = anvil_build_gep(ctx, i32, base, &i, 1, "slot");
    anvil_value_t *used = anvil_build_cmp_ne(ctx, anvil_build_load(ctx, i32, slot, "word"), anvil_const_i32(ctx, 0), "used");
    anvil_build_br_cond(ctx, used, check, join);
    anvil_set_insert_point(ctx, check);
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    anvil_value_t *next_slot = anvil_build_gep(ctx, i32, slot, &one, 1, "next.slot");
    anvil_value_t *match = anvil_build_cmp_eq(ctx, anvil_build_load(ctx, i32, next_slot, "k"), key, "match");
    anvil_build_br_cond(ctx, match, join, latch);
    anvil_set_insert_point(ctx, latch);
    anvil_value_t *next = anvil_build_add(ctx, i, anvil_const_i32(ctx, 2), "next");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(i, anvil_const_i32(ctx, 0), func->entry);
    anvil_phi_add_incoming(i, next, latch);
    anvil_set_insert_point(ctx, join);
    anvil_value_t *two = anvil_const_i32(ctx, 2);
    anvil_value_t *value_at = anvil_build_gep(ctx, i32, slot, &two, 1, "value.at");
    anvil_build_br_cond(ctx, used, hit, miss);
    anvil_set_insert_point(ctx, hit);
    anvil_build_ret(ctx, anvil_build_load(ctx, i32, value_at, "value"));
    anvil_set_insert_point(ctx, miss);
    anvil_build_store(ctx, key, value_at);
    anvil_build_ret(ctx, anvil_const_i32(ctx, -1));

    if (allowance != SIZE_MAX)
        anvil_test_fail_alloc_after(ctx, allowance);
    anvil_pass_result_t status = ANVIL_PASS_RUN_CHANGED;
    for (int round = 0; round < 4 && status == ANVIL_PASS_RUN_CHANGED; round++)
        status = anvil_pass_simplify_cfg(func);
    anvil_test_disable_alloc_fail(ctx);
    anvil_ctx_clear_error(ctx);
    char error[256] = {0};
    bool valid = anvil_func_verify(func, error, sizeof(error));
    *completed = status != ANVIL_PASS_RUN_ERROR;
    if (*completed) {
        /* Only the header still branches on `used`. */
        size_t tests = 0;
        for (anvil_block_t *b = func->blocks; b; b = b->next)
            tests += b->last && b->last->op == ANVIL_OP_BR_COND && b->last->operands[0] == used;
        valid &= tests == 1;
    }
    if (!valid) {
        fprintf(stderr, "jump threading failure: allowance=%zu status=%d error=%s\n", allowance, status, error);
        anvil_dump_module(stderr, module);
    }
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

int main(void)
{
    bool completed = false;
    for (size_t allowance = 0; allowance < 400 && !completed; allowance++)
        if (!run_case(allowance, &completed))
            failures++;
    if (!completed)
        failures++;
    if (failures) {
        fprintf(stderr, "jump_threading_regression: %d failure(s)\n", failures);
        return 1;
    }
    printf("jump_threading_regression: all checks passed\n");
    return 0;
}
