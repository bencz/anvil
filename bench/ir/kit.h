/* Benchmark kernels built directly with the ANVIL API.
 *
 * Each kernel is the IR a good front end would hand over (SSA values and
 * phis, typed GEPs, no stack slots for scalars), so its timing measures
 * ANVIL alone against clang compiling the same algorithm in C (ref/).  The
 * helpers here only wrap the public API. */
#ifndef ANVIL_BENCH_KIT_H
#define ANVIL_BENCH_KIT_H

#include "anvil/anvil.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    anvil_ctx_t *ctx;
    anvil_module_t *mod;
    anvil_type_t *void_type, *i1, *i8, *u8, *i32, *u32, *i64, *f64;
    anvil_type_t *index;   /* the pointer-width signed integer (array indices) */
    anvil_type_t *uindex;  /* its unsigned twin (size_t) */
} bench_t;

/* A function of `name` returning `ret` (NULL: void), with its entry block
 * as the insertion point. */
anvil_func_t *bench_func(bench_t *b, const char *name, anvil_type_t *ret, anvil_type_t **params, size_t num_params);
anvil_value_t *bench_param(anvil_func_t *func, size_t index);

/* A loop in SSA form: open it (its header starts here, the insertion block
 * being the preheader), add the values it carries, test, emit the body,
 * close it with each carried value's next value (in carry order).  Initial
 * values are computed before opening: the header starts with its phis. */
typedef struct {
    bench_t *b;
    anvil_func_t *func;
    anvil_block_t *preheader, *header, *body, *exit;
    anvil_value_t *carried[8];
    size_t num_carried;
} bench_loop_t;

void bench_loop_open(bench_t *b, bench_loop_t *loop, anvil_func_t *func, const char *name);
anvil_value_t *bench_loop_carry(bench_loop_t *loop, anvil_type_t *type, anvil_value_t *initial);
/* Ends the header: the body runs while `cond` holds; insertion moves to it. */
void bench_loop_test(bench_loop_t *loop, anvil_value_t *cond);
/* Ends the body (from the insertion block); insertion moves to the exit. */
void bench_loop_close(bench_loop_t *loop, anvil_value_t **next);

/* Element `index` of an array of `elem` at `base` (a GEP). */
anvil_value_t *bench_elem(bench_t *b, anvil_type_t *elem, anvil_value_t *base, anvil_value_t *index);
anvil_value_t *bench_call(bench_t *b, anvil_func_t *callee, anvil_value_t **args, size_t num_args);
/* `value` as `type` (an index type): sign- or zero-extended when narrower. */
anvil_value_t *bench_to_index(bench_t *b, anvil_value_t *value, anvil_type_t *type, bool sign);
/* A constant of `type`, b->index or b->uindex. */
anvil_value_t *bench_index_const(bench_t *b, anvil_type_t *type, int64_t value);
/* `value` (a signed add, sub or mul: C's int arithmetic) marked no-wrap. */
anvil_value_t *bench_nw(anvil_value_t *value);

typedef bool (*bench_build_fn)(bench_t *b);

typedef struct {
    const char *name;
    bench_build_fn build;
} bench_kernel_t;

extern const bench_kernel_t bench_kernels[];
extern const size_t bench_num_kernels;

#endif
