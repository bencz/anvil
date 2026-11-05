#include "kit.h"

#include <stdio.h>

anvil_func_t *bench_func(bench_t *b, const char *name, anvil_type_t *ret, anvil_type_t **params, size_t num_params)
{
    anvil_type_t *type = anvil_type_func(b->ctx, ret ? ret : b->void_type, params, num_params, false);
    anvil_func_t *func = type ? anvil_func_create(b->mod, name, type, ANVIL_LINK_EXTERNAL) : NULL;
    if (func)
        anvil_set_insert_point(b->ctx, anvil_func_get_entry(func));
    return func;
}

anvil_value_t *bench_param(anvil_func_t *func, size_t index)
{
    return anvil_func_get_param(func, index);
}

void bench_loop_open(bench_t *b, bench_loop_t *loop, anvil_func_t *func, const char *name)
{
    char label[64];
    loop->b = b;
    loop->func = func;
    loop->num_carried = 0;
    loop->preheader = anvil_get_insert_block(b->ctx);
    snprintf(label, sizeof(label), "%s.header", name);
    loop->header = anvil_block_create(func, label);
    snprintf(label, sizeof(label), "%s.body", name);
    loop->body = anvil_block_create(func, label);
    snprintf(label, sizeof(label), "%s.exit", name);
    loop->exit = anvil_block_create(func, label);
    anvil_build_br(b->ctx, loop->header);
    anvil_set_insert_point(b->ctx, loop->header);
}

anvil_value_t *bench_loop_carry(bench_loop_t *loop, anvil_type_t *type, anvil_value_t *initial)
{
    if (loop->num_carried == sizeof(loop->carried) / sizeof(loop->carried[0]))
        return NULL;
    anvil_value_t *phi = anvil_build_phi(loop->b->ctx, type, "carried");
    if (phi)
        anvil_phi_add_incoming(phi, initial, loop->preheader);
    loop->carried[loop->num_carried++] = phi;
    return phi;
}

void bench_loop_test(bench_loop_t *loop, anvil_value_t *cond)
{
    anvil_build_br_cond(loop->b->ctx, cond, loop->body, loop->exit);
    anvil_set_insert_point(loop->b->ctx, loop->body);
}

void bench_loop_close(bench_loop_t *loop, anvil_value_t **next)
{
    anvil_block_t *latch = anvil_get_insert_block(loop->b->ctx);
    for (size_t i = 0; i < loop->num_carried; i++)
        anvil_phi_add_incoming(loop->carried[i], next[i], latch);
    anvil_build_br(loop->b->ctx, loop->header);
    anvil_set_insert_point(loop->b->ctx, loop->exit);
}

anvil_value_t *bench_elem(bench_t *b, anvil_type_t *elem, anvil_value_t *base, anvil_value_t *index)
{
    anvil_value_t *indices[] = { index };
    return anvil_build_gep(b->ctx, elem, base, indices, 1, "elem");
}

anvil_value_t *bench_call(bench_t *b, anvil_func_t *callee, anvil_value_t **args, size_t num_args)
{
    anvil_value_t *result = NULL;
    return anvil_build_call_checked(b->ctx, anvil_func_get_value(callee), args, num_args, "call", &result) ? result : NULL;
}

anvil_value_t *bench_to_index(bench_t *b, anvil_value_t *value, anvil_type_t *type, bool sign)
{
    if (anvil_type_size(anvil_value_get_type(value)) == anvil_type_size(type))
        return value;
    return sign ? anvil_build_sext(b->ctx, value, type, "wide") : anvil_build_zext(b->ctx, value, type, "wide");
}

anvil_value_t *bench_index_const(bench_t *b, anvil_type_t *type, int64_t value)
{
    bool wide = b->index == b->i64;
    if (type == b->uindex)
        return wide ? anvil_const_u64(b->ctx, (uint64_t)value) : anvil_const_u32(b->ctx, (uint32_t)value);
    return wide ? anvil_const_i64(b->ctx, value) : anvil_const_i32(b->ctx, (int32_t)value);
}

anvil_value_t *bench_nw(anvil_value_t *value)
{
    anvil_set_no_wrap(value, true);
    return value;
}
