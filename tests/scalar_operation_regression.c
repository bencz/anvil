/* Table-priced scalar operations and cold placement: FSQRT runs as the
 * square-root instruction each target's CPU model has (and is priced 0
 * where it has none), folds on IEEE constants and packs into lanes;
 * division by constants becomes a multiply-high sequence that computes the
 * exact quotient and remainder; cold blocks follow the rest of the
 * function. */
#include <anvil/anvil_internal.h>
#include <anvil/anvil_analysis.h>
#include <anvil/anvil_debug.h>
#include <anvil/anvil_opt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    anvil_arch_t arch;
    anvil_cpu_model_t cpu;   /* 0: the architecture's default */
    const char *scalar;      /* expected mnemonic, NULL: priced 0 */
    const char *lanes;       /* v2f64 form, NULL: none */
} sqrt_case_t;

static const sqrt_case_t cases[] = {
    {ANVIL_ARCH_X86_64, 0, "sqrtsd", "sqrtpd"},
    {ANVIL_ARCH_X86, ANVIL_CPU_X86_I486, "fsqrt", NULL},
    {ANVIL_ARCH_X86, ANVIL_CPU_X86_PENTIUM4, "sqrtsd", NULL},
    {ANVIL_ARCH_ARM64, 0, "fsqrt d", "fsqrt v"},
    {ANVIL_ARCH_PPC64, ANVIL_CPU_PPC64_POWER8, "fsqrt ", "xvsqrtdp"},
    {ANVIL_ARCH_PPC64LE, ANVIL_CPU_PPC64_POWER8, "fsqrt ", "xvsqrtdp"},
    {ANVIL_ARCH_PPC64, ANVIL_CPU_PPC64_970, "fsqrt ", NULL},
    {ANVIL_ARCH_PPC32, ANVIL_CPU_PPC_G4, NULL, NULL},
    {ANVIL_ARCH_PPC32, ANVIL_CPU_PPC_G3, NULL, NULL},
    {ANVIL_ARCH_ZARCH, 0, NULL, NULL},
};

static anvil_ctx_t *target_ctx(const sqrt_case_t *target)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(target->arch);
    if (ctx && target->cpu)
        anvil_ctx_set_cpu(ctx, target->cpu);
    return ctx;
}

/* double root(double x) { return fsqrt(x); }, and its lane form. */
static bool run_sqrt_case(const sqrt_case_t *target, bool vector)
{
    anvil_ctx_t *ctx = target_ctx(target);
    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_type_t *type = vector ? anvil_type_vector(ctx, f64, 2) : f64;
    const char *expected = vector ? target->lanes : target->scalar;
    unsigned cost = anvil_vector_shape_cost(ctx, ANVIL_OP_FSQRT, type, vector ? ANVIL_VECTOR_SHAPE_LANES : ANVIL_VECTOR_SHAPE_SCALAR);
    bool valid = (cost != 0) == (expected != NULL);
    char *assembly = NULL;
    size_t length = 0;
    if (valid && expected) {
        anvil_module_t *module = anvil_module_create(ctx, "sqrt");
        anvil_type_t *pointer = anvil_type_ptr(ctx, type);
        anvil_type_t *params[] = {vector ? pointer : type};
        anvil_func_t *func = anvil_func_create(module, "root", anvil_type_func(ctx, vector ? anvil_type_void(ctx) : type, params, 1, false),
                                               ANVIL_LINK_EXTERNAL);
        anvil_set_insert_point(ctx, func->entry);
        anvil_value_t *param = anvil_func_get_param(func, 0);
        if (vector) {
            anvil_value_t *lanes = anvil_build_load(ctx, type, param, "lanes");
            anvil_build_store(ctx, anvil_build_fsqrt(ctx, lanes, "roots"), param);
            anvil_build_ret_void(ctx);
        } else {
            anvil_build_ret(ctx, anvil_build_fsqrt(ctx, param, "root"));
        }
        valid = anvil_func_verify(func, NULL, 0) && anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly &&
                strstr(assembly, expected);
        anvil_module_destroy(module);
    }
    if (!valid)
        fprintf(stderr, "sqrt failure: arch=%d cpu=%d vector=%d cost=%u expected=%s\n%s\n", (int)target->arch, (int)target->cpu, vector,
                cost, expected ? expected : "(none)", assembly ? assembly : "");
    free(assembly);
    anvil_ctx_destroy(ctx);
    return valid;
}

/* fsqrt of a non-negative IEEE constant folds; a negative one stays (its
 * NaN is the target's). */
static bool run_fold_case(double operand, bool folds, double expected)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "fold");
    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_func_t *func = anvil_func_create(module, "constant", anvil_type_func(ctx, f64, NULL, 0, false), ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, func->entry);
    anvil_build_ret(ctx, anvil_build_fsqrt(ctx, anvil_const_f64(ctx, operand), "root"));
    anvil_pass_result_t status = anvil_pass_const_fold(func);
    anvil_instr_t *ret = func->entry->last;
    anvil_value_t *value = ret && ret->num_operands == 1 ? ret->operands[0] : NULL;
    bool folded = value && value->kind == ANVIL_VAL_CONST_FLOAT;
    bool valid = status != ANVIL_PASS_RUN_ERROR && anvil_func_verify(func, NULL, 0) && folded == folds && (!folded || value->data.f == expected);
    if (!valid)
        fprintf(stderr, "sqrt fold failure: operand=%g folds=%d\n", operand, folds);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

/* Division by a constant: strength_reduce rewrites `n op d` into the
 * multiply-high sequence (no division left), and const_fold evaluates that
 * sequence to exactly n / d or n % d for every operand pair. */
static anvil_value_t *division_constant(anvil_ctx_t *ctx, unsigned bits, bool is_signed, uint64_t value)
{
    if (bits == 32)
        return is_signed ? anvil_const_i32(ctx, (int32_t)value) : anvil_const_u32(ctx, (uint32_t)value);
    return is_signed ? anvil_const_i64(ctx, (int64_t)value) : anvil_const_u64(ctx, value);
}

static bool run_division_case(anvil_type_t *(*make_type)(anvil_ctx_t *), unsigned bits, bool is_signed)
{
    static const int64_t divisors[] = {3, 5, 6, 7, 10, 11, 12, 25, 60, 100, 641, 1000, 6700417, 1000000007, 0x7fffffff, -3, -7, -10, -1000};
    static const uint64_t dividends[] = {0, 1, 2, 6, 7, 9, 10, 99, 1000, 0x7ffffffe, 0x7fffffff, 0x80000000u, 0xfffffffeu, 0xffffffffu,
                                         0x123456789abcdefull, 0x7fffffffffffffffull, 0x8000000000000001ull, 0xfffffffffffffff9ull,
                                         0xffffffffffffffffull};
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "division");
    anvil_type_t *type = make_type(ctx);
    uint64_t mask = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1, sign = UINT64_C(1) << (bits - 1);
    bool valid = true;
    for (size_t di = 0; valid && di < sizeof(divisors) / sizeof(divisors[0]); di++) {
        uint64_t d = (uint64_t)divisors[di] & mask;
        for (size_t ni = 0; valid && ni < sizeof(dividends) / sizeof(dividends[0]); ni++) {
            for (int remainder = 0; valid && remainder < 2; remainder++) {
                uint64_t n = dividends[ni] & mask, expected;
                if (is_signed) {
                    int64_t sn = (int64_t)(n & sign ? n | ~mask : n), sd = (int64_t)(d & sign ? d | ~mask : d);
                    expected = (uint64_t)(remainder ? sn % sd : sn / sd) & mask;
                } else {
                    expected = remainder ? n % d : n / d;
                }
                char name[48];
                snprintf(name, sizeof(name), "divide_%zu_%zu_%d", di, ni, remainder);
                anvil_func_t *func = anvil_func_create(module, name, anvil_type_func(ctx, type, NULL, 0, false), ANVIL_LINK_INTERNAL);
                if (!(valid = func != NULL))
                    break;
                anvil_set_insert_point(ctx, func->entry);
                anvil_value_t *left = division_constant(ctx, bits, is_signed, n), *right = division_constant(ctx, bits, is_signed, d);
                anvil_op_t op = is_signed ? (remainder ? ANVIL_OP_SMOD : ANVIL_OP_SDIV) : (remainder ? ANVIL_OP_UMOD : ANVIL_OP_UDIV);
                anvil_value_t *result = op == ANVIL_OP_SMOD ? anvil_build_smod(ctx, left, right, "r")
                                        : op == ANVIL_OP_SDIV ? anvil_build_sdiv(ctx, left, right, "q")
                                        : op == ANVIL_OP_UMOD ? anvil_build_umod(ctx, left, right, "r")
                                                              : anvil_build_udiv(ctx, left, right, "q");
                anvil_build_ret(ctx, result);
                bool divides = false;
                valid = anvil_pass_strength_reduce(func) == ANVIL_PASS_RUN_CHANGED;
                for (anvil_instr_t *instr = func->entry->first; valid && instr; instr = instr->next)
                    divides |= instr->op == op;
                while (valid && anvil_pass_const_fold(func) == ANVIL_PASS_RUN_CHANGED) {
                }
                anvil_instr_t *ret = func->entry->last;
                anvil_value_t *value = ret && ret->num_operands == 1 ? ret->operands[0] : NULL;
                valid = valid && !divides && anvil_func_verify(func, NULL, 0) && value && value->kind == ANVIL_VAL_CONST_INT &&
                        (anvil_const_int_unsigned_value(value) & mask) == expected;
                if (!valid) {
                    fprintf(stderr, "division failure: bits=%u signed=%d remainder=%d n=%llx d=%llx expected=%llx\n", bits, is_signed, remainder,
                            (unsigned long long)n, (unsigned long long)d, (unsigned long long)expected);
                    anvil_dump_func(stderr, func);
                }
            }
        }
    }
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

/* (x << k) | (x >> (32 - k)) becomes a rotate where the target has one
 * (constant and variable counts), a select of integers a conditional move
 * fused with its compare, and both functions are leaves without a frame. */
typedef struct {
    anvil_arch_t arch;
    anvil_cpu_model_t cpu;
    const char *rotate_constant, *rotate_variable, *select, *frame;  /* frame: must not appear */
} lowering_case_t;

static const lowering_case_t lowering_cases[] = {
    {ANVIL_ARCH_X86_64, 0, "roll $5", "roll %cl", "cmovg", "pushq %rbp"},
    {ANVIL_ARCH_ARM64, 0, "ror w0, w0, #27", "neg w16", "csel w0", "stp x29"},
    {ANVIL_ARCH_PPC64LE, ANVIL_CPU_PPC64_POWER8, "rotlwi", "rotlw ", "isel", "mflr"},
    {ANVIL_ARCH_PPC64, ANVIL_CPU_PPC64_POWER4, "rotlwi", "rotlw ", "bc ", "mflr"},
};

static bool run_lowering_case(const lowering_case_t *test)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(test->arch);
    if (test->cpu)
        anvil_ctx_set_cpu(ctx, test->cpu);
    anvil_module_t *module = anvil_module_create(ctx, "lowering");
    anvil_type_t *u32 = anvil_type_u32(ctx), *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = {u32, u32};
    anvil_func_t *constant = anvil_func_create(module, "rotate_constant", anvil_type_func(ctx, u32, params, 1, false), ANVIL_LINK_EXTERNAL);
    anvil_func_t *variable = anvil_func_create(module, "rotate_variable", anvil_type_func(ctx, u32, params, 2, false), ANVIL_LINK_EXTERNAL);
    anvil_type_t *pick_params[] = {i32, i32};
    anvil_func_t *pick = anvil_func_create(module, "maximum", anvil_type_func(ctx, i32, pick_params, 2, false), ANVIL_LINK_EXTERNAL);
    anvil_func_t *funcs[] = {constant, variable};
    for (size_t index = 0; index < 2; index++) {
        anvil_func_t *func = funcs[index];
        anvil_set_insert_point(ctx, func->entry);
        anvil_value_t *x = anvil_func_get_param(func, 0);
        anvil_value_t *k = index ? anvil_func_get_param(func, 1) : anvil_const_u32(ctx, 5);
        anvil_value_t *rest = index ? anvil_build_sub(ctx, anvil_const_u32(ctx, 32), k, "rest") : anvil_const_u32(ctx, 27);
        anvil_value_t *left = anvil_build_shl(ctx, x, k, "left"), *right = anvil_build_shr(ctx, x, rest, "right");
        anvil_build_ret(ctx, anvil_build_or(ctx, left, right, "rotated"));
    }
    anvil_set_insert_point(ctx, pick->entry);
    anvil_value_t *a = anvil_func_get_param(pick, 0), *b = anvil_func_get_param(pick, 1);
    anvil_build_ret(ctx, anvil_build_select(ctx, anvil_build_cmp_gt(ctx, a, b, "greater"), a, b, "maximum"));

    bool rotated = anvil_pass_strength_reduce(constant) == ANVIL_PASS_RUN_CHANGED && anvil_pass_strength_reduce(variable) == ANVIL_PASS_RUN_CHANGED &&
                   constant->entry->last->operands[0]->data.instr->op == ANVIL_OP_ROTL &&
                   variable->entry->last->operands[0]->data.instr->op == ANVIL_OP_ROTL;
    char *assembly = NULL;
    size_t length = 0;
    bool valid = rotated && anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly && strstr(assembly, test->rotate_constant) &&
                 strstr(assembly, test->rotate_variable) && strstr(assembly, test->select) && !strstr(assembly, test->frame);
    if (!valid)
        fprintf(stderr, "lowering failure: arch=%d rotated=%d\n%s\n", (int)test->arch, rotated, assembly ? assembly : "");
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

/* for (i = 0; i < n; i++) s += v[i].b;  with sizeof(*v) = 24: the address
 * becomes a pointer stream advancing 24 bytes per iteration (no multiply);
 * an 8-byte element keeps its scaled index, and the bounded int index is
 * widened (no extension per iteration); i != n (no bound) with a narrow
 * index stays as it is. */
static bool run_loop_strength_case(anvil_arch_t arch, unsigned fields, bool bounded, bool expect_stream)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    anvil_module_t *module = anvil_module_create(ctx, "strength");
    anvil_type_t *i64 = anvil_type_i64(ctx), *i32 = anvil_type_i32(ctx);
    anvil_type_t *members[] = {i64, i64, i64};
    anvil_type_t *record = fields == 1 ? i64 : anvil_type_struct(ctx, "record", members, fields);
    anvil_type_t *params[] = {anvil_type_ptr(ctx, record), i32};
    anvil_func_t *func = anvil_func_create(module, "sum", anvil_type_func(ctx, i64, params, 2, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *index = anvil_build_phi(ctx, i32, "i"), *total = anvil_build_phi(ctx, i64, "total");
    anvil_value_t *more = bounded ? anvil_build_cmp_lt(ctx, index, anvil_func_get_param(func, 1), "more")
                                  : anvil_build_cmp_ne(ctx, index, anvil_func_get_param(func, 1), "more");
    anvil_build_br_cond(ctx, more, body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *indices[] = {index, anvil_const_i32(ctx, 1)};
    anvil_value_t *address = anvil_build_gep(ctx, record, anvil_func_get_param(func, 0), indices, fields == 1 ? 1 : 2, "field");
    anvil_value_t *sum = anvil_build_add(ctx, total, anvil_build_load(ctx, i64, address, "value"), "sum");
    anvil_value_t *next = anvil_build_add(ctx, index, anvil_const_i32(ctx, 1), "next");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(index, anvil_const_i32(ctx, 0), func->entry);
    anvil_phi_add_incoming(index, next, body);
    anvil_phi_add_incoming(total, anvil_const_i64(ctx, 0), func->entry);
    anvil_phi_add_incoming(total, sum, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_ret(ctx, total);

    anvil_pass_result_t status = anvil_pass_loop_strength(func);
    bool cleaned = anvil_pass_dce(func) != ANVIL_PASS_RUN_ERROR;
    char error[256] = {0};
    char *assembly = NULL;
    size_t length = 0;
    bool valid = cleaned && anvil_func_verify(func, error, sizeof(error)) && status == (bounded ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED) &&
                 anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly && (!expect_stream || !strstr(assembly, "imul"));
    /* Widened: n is extended once, before the loop, and nothing else. */
    const char *first = valid && bounded && arch == ANVIL_ARCH_X86_64 ? strstr(assembly, "movslq") : NULL;
    valid = valid && (!first || !strstr(first + 1, "movslq") || !bounded);
    if (!valid) {
        fprintf(stderr, "loop strength failure: arch=%d fields=%u bounded=%d status=%d error=%s\n%s\n", (int)arch, fields, bounded, status, error,
                assembly ? assembly : "");
        anvil_dump_module(stderr, module);
    }
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

/* fib(n) = n < 2 ? n : fib(n - 1) + fib(n - 2): the tail call becomes a
 * jump back with n - 2, fib(n - 1) accumulating (one self-call left);
 * with an alloca in the function the recursion keeps its frames. */
static bool run_tail_recursion_case(bool with_alloca)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "recursion");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = {i32};
    anvil_func_t *func = anvil_func_create(module, "fib", anvil_type_func(ctx, i32, params, 1, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *base = anvil_block_create(func, "base"), *recurse = anvil_block_create(func, "recurse"), *done = anvil_block_create(func, "done");
    anvil_value_t *n = anvil_func_get_param(func, 0);
    anvil_set_insert_point(ctx, func->entry);
    if (with_alloca)
        anvil_build_store(ctx, n, anvil_build_alloca(ctx, i32, "slot"));
    anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, n, anvil_const_i32(ctx, 2), "small"), base, recurse);
    anvil_set_insert_point(ctx, base);
    anvil_build_br(ctx, done);
    anvil_set_insert_point(ctx, recurse);
    anvil_value_t *first = NULL, *second = NULL;
    anvil_value_t *arg1 = anvil_build_sub(ctx, n, anvil_const_i32(ctx, 1), "n1");
    anvil_build_call_checked(ctx, func->value, &arg1, 1, "first", &first);
    anvil_value_t *arg2 = anvil_build_sub(ctx, n, anvil_const_i32(ctx, 2), "n2");
    anvil_build_call_checked(ctx, func->value, &arg2, 1, "second", &second);
    anvil_value_t *sum = anvil_build_add(ctx, first, second, "sum");
    anvil_build_br(ctx, done);
    anvil_set_insert_point(ctx, done);
    anvil_value_t *result = anvil_build_phi(ctx, i32, "result");
    anvil_phi_add_incoming(result, n, base);
    anvil_phi_add_incoming(result, sum, recurse);
    anvil_build_ret(ctx, result);

    anvil_pass_result_t status = anvil_pass_tail_recursion(func);
    size_t calls = 0;
    for (anvil_block_t *block = func->blocks; block; block = block->next)
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
            calls += instr->op == ANVIL_OP_CALL;
    char error[256] = {0};
    char *assembly = NULL;
    size_t length = 0;
    bool valid = anvil_func_verify(func, error, sizeof(error)) && status == (with_alloca ? ANVIL_PASS_RUN_UNCHANGED : ANVIL_PASS_RUN_CHANGED) &&
                 calls == (with_alloca ? 2u : 1u) && anvil_module_codegen(module, &assembly, &length) == ANVIL_OK;
    if (!valid) {
        fprintf(stderr, "tail recursion failure: alloca=%d status=%d calls=%zu error=%s\n", with_alloca, status, calls, error);
        anvil_dump_module(stderr, module);
    }
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

/* entry -> (rare | hot); rare -> after_rare -> join; hot -> join.  The
 * cold block and the block only it leads to are laid out last. */
static bool run_cold_layout_case(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "cold");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = {i32};
    anvil_func_t *func = anvil_func_create(module, "cold", anvil_type_func(ctx, i32, params, 1, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *rare = anvil_block_create(func, "rare");
    anvil_block_t *after_rare = anvil_block_create(func, "after_rare");
    anvil_block_t *hot = anvil_block_create(func, "hot");
    anvil_block_t *join = anvil_block_create(func, "join");
    anvil_block_set_cold(rare, true);
    anvil_set_insert_point(ctx, func->entry);
    anvil_value_t *zero = anvil_build_cmp_eq(ctx, anvil_func_get_param(func, 0), anvil_const_i32(ctx, 0), "zero");
    anvil_build_br_cond(ctx, zero, rare, hot);
    anvil_set_insert_point(ctx, rare);
    anvil_build_br(ctx, after_rare);
    anvil_set_insert_point(ctx, after_rare);
    anvil_build_br(ctx, join);
    anvil_set_insert_point(ctx, hot);
    anvil_build_br(ctx, join);
    anvil_set_insert_point(ctx, join);
    anvil_build_ret(ctx, anvil_func_get_param(func, 0));

    anvil_opt_cfg_t cfg;
    bool valid = anvil_func_verify(func, NULL, 0) && anvil_opt_cfg_build(func, &cfg);
    if (valid) {
        valid = anvil_opt_cfg_build_layout(&cfg) && cfg.count == 5;
        const anvil_block_t *expected[] = {func->entry, hot, join, rare, after_rare};
        for (size_t position = 0; valid && position < 5; position++)
            valid = cfg.blocks[cfg.layout[position]] == expected[position];
        anvil_opt_cfg_destroy(&cfg);
    }
    char *assembly = NULL;
    size_t length = 0;
    valid = valid && anvil_module_codegen(module, &assembly, &length) == ANVIL_OK;
    if (!valid)
        fprintf(stderr, "cold layout failure\n%s\n", assembly ? assembly : "");
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

int main(void)
{
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        if (!run_sqrt_case(&cases[index], false) || !run_sqrt_case(&cases[index], true))
            return 1;
    }
    if (!run_tail_recursion_case(false) || !run_tail_recursion_case(true))
        return 1;
    if (!run_loop_strength_case(ANVIL_ARCH_X86_64, 3, true, true) || !run_loop_strength_case(ANVIL_ARCH_ARM64, 3, true, true) ||
        !run_loop_strength_case(ANVIL_ARCH_X86_64, 1, true, false) || !run_loop_strength_case(ANVIL_ARCH_X86_64, 3, false, false))
        return 1;
    for (size_t index = 0; index < sizeof(lowering_cases) / sizeof(lowering_cases[0]); index++) {
        if (!run_lowering_case(&lowering_cases[index]))
            return 1;
    }
    if (!run_division_case(anvil_type_u32, 32, false) || !run_division_case(anvil_type_i32, 32, true) ||
        !run_division_case(anvil_type_u64, 64, false) || !run_division_case(anvil_type_i64, 64, true))
        return 1;
    if (!run_fold_case(6.25, true, 2.5) || !run_fold_case(-0.0, true, -0.0) || !run_fold_case(-4.0, false, 0.0) || !run_cold_layout_case())
        return 1;
    printf("scalar operation regression tests passed\n");
    return 0;
}
