/* Jump threading on an earlier test of the same condition: a loop leaving
 * on c (false) or after c (true) reaches a join that tests c again, holding
 * pure address work its successors read.  The work moves into both
 * successors and each exit edge goes straight to the side c chooses, so the
 * loop no longer keeps c as a value.
 * Jump threading on a known outcome whose PHIs are read past the branch: a
 * loop whose latch brings the counter value that leaves it (what a closed
 * form leaves) loses its back edge, the exit reading new PHIs of the
 * header's values; PHIs swapping through a block the threaded edge reaches
 * read values there that the skipped join never defined, so they are
 * repaired too or the edge stays.  Allocation failure at any point leaves
 * valid IR. */
#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include "anvil/anvil_debug.h"
#include <stdio.h>
#include <stdlib.h>

static int failures;
static const anvil_value_t *used_condition;

static bool finish_case(anvil_ctx_t *ctx, anvil_module_t *module, anvil_func_t *func, size_t allowance, bool *completed, const char *name,
                        bool (*expect)(anvil_func_t *func));

/* Only the header still branches on `used`. */
static bool only_header_tests(anvil_func_t *func)
{
    size_t tests = 0;
    for (anvil_block_t *b = func->blocks; b; b = b->next)
        tests += b->last && b->last->op == ANVIL_OP_BR_COND && b->last->operands[0] == used_condition;
    return tests == 1;
}

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

    used_condition = used;
    return finish_case(ctx, module, func, allowance, completed, "jump threading", only_header_tests);
}

static bool finish_case(anvil_ctx_t *ctx, anvil_module_t *module, anvil_func_t *func, size_t allowance, bool *completed, const char *name,
                        bool (*expect)(anvil_func_t *func))
{
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
    if (*completed && valid)
        valid = expect(func);
    if (!valid) {
        fprintf(stderr, "%s failure: allowance=%zu status=%d error=%s\n", name, allowance, status, error);
        anvil_dump_module(stderr, module);
    }
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

static size_t predecessors_of(anvil_func_t *func, const anvil_block_t *block)
{
    size_t count = 0;
    for (anvil_block_t *b = func->blocks; b; b = b->next) {
        const anvil_instr_t *term = b->last;
        if (term && term->op == ANVIL_OP_BR)
            count += term->true_block == block;
        else if (term && term->op == ANVIL_OP_BR_COND)
            count += (term->true_block == block) + (term->false_block == block);
    }
    return count;
}

/* The header keeps only its entry edge; the exit merges both paths. */
static bool one_trip_expected(anvil_func_t *func)
{
    anvil_block_t *header = func->entry->next, *exit = NULL;
    for (anvil_block_t *b = func->blocks; b; b = b->next)
        if (b->last && b->last->op == ANVIL_OP_RET)
            exit = b;
    return header && exit && predecessors_of(func, header) == 1 && predecessors_of(func, exit) == 2 && exit->first->op == ANVIL_OP_PHI;
}

/*   entry: br header
 *   header: n = phi [c, entry], [0, body]; s = phi [10, entry], [s2, body]
 *           br n != 0, body, exit
 *   body:   s2 = s + n; store s2, p; br header
 *   exit:   ret s + n */
static bool run_one_trip_case(size_t allowance, bool *completed)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "one_trip");
    anvil_type_t *i32 = anvil_type_i32(ctx), *p32 = anvil_type_ptr(ctx, i32);
    anvil_type_t *params[] = {p32, i32};
    anvil_func_t *func = anvil_func_create(module, "drain", anvil_type_func(ctx, i32, params, 2, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body");
    anvil_block_t *exit = anvil_block_create(func, "exit");
    anvil_value_t *pointer = anvil_func_get_param(func, 0), *count = anvil_func_get_param(func, 1);

    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *n = anvil_build_phi(ctx, i32, "n"), *sum = anvil_build_phi(ctx, i32, "s");
    anvil_build_br_cond(ctx, anvil_build_cmp_ne(ctx, n, anvil_const_i32(ctx, 0), "more"), body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *next = anvil_build_add(ctx, sum, n, "s2");
    anvil_build_store(ctx, next, pointer);
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(n, count, func->entry);
    anvil_phi_add_incoming(n, anvil_const_i32(ctx, 0), body);
    anvil_phi_add_incoming(sum, anvil_const_i32(ctx, 10), func->entry);
    anvil_phi_add_incoming(sum, next, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_ret(ctx, anvil_build_add(ctx, sum, n, "result"));
    return finish_case(ctx, module, func, allowance, completed, "one-trip threading", one_trip_expected);
}

static bool any_valid(anvil_func_t *func)
{
    (void)func;
    return true;
}

/*   entry: br join
 *   join: a = phi [0, entry], [b, again]; b = phi [1, entry], [a, again]
 *         br a != 0, done, side
 *   side: x = load p; br again
 *   again: br x != 0, join, out
 *   done: ret 7        out: ret 9
 * Threading entry -> side, `again` reads a and b without join in between. */
static bool run_swap_case(size_t allowance, bool *completed)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "swap");
    anvil_type_t *i32 = anvil_type_i32(ctx), *p32 = anvil_type_ptr(ctx, i32);
    anvil_type_t *params[] = {p32};
    anvil_func_t *func = anvil_func_create(module, "swap", anvil_type_func(ctx, i32, params, 1, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *join = anvil_block_create(func, "join"), *side = anvil_block_create(func, "side");
    anvil_block_t *again = anvil_block_create(func, "again"), *done = anvil_block_create(func, "done");
    anvil_block_t *out = anvil_block_create(func, "out");
    anvil_value_t *pointer = anvil_func_get_param(func, 0);

    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, join);
    anvil_set_insert_point(ctx, join);
    anvil_value_t *a = anvil_build_phi(ctx, i32, "a"), *b = anvil_build_phi(ctx, i32, "b");
    anvil_build_br_cond(ctx, anvil_build_cmp_ne(ctx, a, anvil_const_i32(ctx, 0), "set"), done, side);
    anvil_set_insert_point(ctx, side);
    anvil_value_t *x = anvil_build_load(ctx, i32, pointer, "x");
    anvil_build_br(ctx, again);
    anvil_set_insert_point(ctx, again);
    anvil_build_br_cond(ctx, anvil_build_cmp_ne(ctx, x, anvil_const_i32(ctx, 0), "loop"), join, out);
    anvil_phi_add_incoming(a, anvil_const_i32(ctx, 0), func->entry);
    anvil_phi_add_incoming(a, b, again);
    anvil_phi_add_incoming(b, anvil_const_i32(ctx, 1), func->entry);
    anvil_phi_add_incoming(b, a, again);
    anvil_set_insert_point(ctx, done);
    anvil_build_ret(ctx, anvil_const_i32(ctx, 7));
    anvil_set_insert_point(ctx, out);
    anvil_build_ret(ctx, anvil_const_i32(ctx, 9));
    return finish_case(ctx, module, func, allowance, completed, "swap threading", any_valid);
}

static void sweep(bool (*run)(size_t allowance, bool *completed))
{
    bool completed = false;
    for (size_t allowance = 0; allowance < 400 && !completed; allowance++)
        if (!run(allowance, &completed))
            failures++;
    if (!completed)
        failures++;
}

int main(void)
{
    sweep(run_case);
    sweep(run_one_trip_case);
    sweep(run_swap_case);
    if (failures) {
        fprintf(stderr, "jump_threading_regression: %d failure(s)\n", failures);
        return 1;
    }
    printf("jump_threading_regression: all checks passed\n");
    return 0;
}
