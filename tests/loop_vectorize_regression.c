/* Loop vectorizer contracts: a counted unit-stride loop gets a vector copy
 * on every target whose vector costs accept it, stays scalar where policy or
 * shape forbids it, and allocation failure at any point leaves valid IR. */
#include <anvil/anvil_internal.h>
#include <anvil/anvil_opt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    VECTORIZED,       /* f32: c[i] = a[i] * k + b[i] */
    INTEGER,          /* i32: c[i] = (a[i] ^ b[i]) + 5 */
    UNIFORM_SHIFT,    /* i32: c[i] = a[i] >> k (arithmetic) */
    SELECT_MAX,       /* i32: c[i] = a[i] > b[i] ? a[i] : b[i] */
    SUM,              /* i32: s += a[i], returned through c[0] */
    NO_FP_POLICY,
    VOLATILE_LOAD,
    INDEX_AS_VALUE,
    SHORT_TRIP,       /* three iterations: the cost model keeps it scalar */
    MODE_COUNT
};

typedef struct {
    anvil_arch_t arch;
    anvil_cpu_model_t cpu;      /* 0: the architecture's default */
    const char *expected[5][2]; /* VECTORIZED, INTEGER, UNIFORM_SHIFT, SELECT_MAX, SUM */
} target_case_t;

static const target_case_t targets[] = {
    {ANVIL_ARCH_X86_64, 0, {{"mulps", "shufps"}, {"pxor", "paddd"}, {"psrad", "movd"}, {"pcmpgtd", "pandn"}, {"paddd", "pshufd $0x4e"}}},
    {ANVIL_ARCH_ARM64, 0, {{"fmul v", "dup v"}, {"eor v", "add v"}, {"sshl v", "neg v"}, {"cmgt v", ".16b"}, {"add v", "addv"}}},
    {ANVIL_ARCH_PPC64, ANVIL_CPU_PPC64_POWER8, {{"xvmulsp", "xxspltw"}, {"vxor", "vadduwm"}, {"vsraw", "mtvsrwz"}, {"vcmpgtsw", "xxsel"}, {"vadduwm", "vsldoi"}}},
    {ANVIL_ARCH_PPC64LE, ANVIL_CPU_PPC64_POWER8, {{"xvmulsp", "xxspltw"}, {"vxor", "vadduwm"}, {"vsraw", "mtvsrwz"}, {"vcmpgtsw", "xxsel"}, {"vadduwm", "vsldoi"}}},
};

static bool vectorizes(unsigned mode)
{
    return mode == VECTORIZED || mode == INTEGER || mode == UNIFORM_SHIFT || mode == SELECT_MAX || mode == SUM;
}

static bool run_case(const target_case_t *target, unsigned mode, size_t allowance, bool *completed)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(target->arch);
    if (target->cpu)
        anvil_ctx_set_cpu(ctx, target->cpu);
    anvil_module_t *module = anvil_module_create(ctx, "loop_vectorize");
    bool integer = mode == INTEGER || mode == UNIFORM_SHIFT || mode == SELECT_MAX || mode == SUM;
    anvil_type_t *element = integer ? anvil_type_i32(ctx) : anvil_type_f32(ctx);
    anvil_type_t *pointer = anvil_type_ptr(ctx, element);
    anvil_type_t *index_type = anvil_type_i32(ctx);
    anvil_type_t *params[] = {pointer, pointer, pointer, element, index_type};
    anvil_type_t *type = anvil_type_func(ctx, anvil_type_void(ctx), params, 5, false);
    anvil_func_t *func = anvil_func_create(module, "fma_loop", type, ANVIL_LINK_EXTERNAL);
    anvil_func_set_fp_vectorization(func, mode != NO_FP_POLICY);
    anvil_block_t *header = anvil_block_create(func, "header");
    anvil_block_t *body = anvil_block_create(func, "body");
    anvil_block_t *exit = anvil_block_create(func, "exit");

    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);

    anvil_set_insert_point(ctx, header);
    anvil_value_t *index = anvil_build_phi(ctx, index_type, "i");
    anvil_value_t *total = mode == SUM ? anvil_build_phi(ctx, element, "total") : NULL;
    anvil_value_t *limit = mode == SHORT_TRIP ? anvil_const_i32(ctx, 3) : anvil_func_get_param(func, 4);
    anvil_value_t *more = anvil_build_cmp_lt(ctx, index, limit, "more");
    anvil_build_br_cond(ctx, more, body, exit);

    anvil_set_insert_point(ctx, body);
    anvil_memory_access_t access = {.is_volatile = mode == VOLATILE_LOAD};
    anvil_value_t *a_address = anvil_build_gep(ctx, element, anvil_func_get_param(func, 0), &index, 1, "a.address");
    anvil_value_t *b_address = anvil_build_gep(ctx, element, anvil_func_get_param(func, 1), &index, 1, "b.address");
    anvil_value_t *c_address = anvil_build_gep(ctx, element, anvil_func_get_param(func, 2), &index, 1, "c.address");
    anvil_value_t *a = anvil_build_load_ex(ctx, element, a_address, &access, "a");
    anvil_value_t *b = mode == INDEX_AS_VALUE ? anvil_build_sitofp(ctx, index, element, "b")
                                               : anvil_build_load(ctx, element, b_address, "b");
    anvil_value_t *k = anvil_func_get_param(func, 3);
    anvil_value_t *sum;
    if (mode == INTEGER)
        sum = anvil_build_add(ctx, anvil_build_xor(ctx, a, b, "mixed"), anvil_const_i32(ctx, 5), "sum");
    else if (mode == UNIFORM_SHIFT)
        sum = anvil_build_sar(ctx, a, k, "sum");
    else if (mode == SELECT_MAX)
        sum = anvil_build_select(ctx, anvil_build_cmp_gt(ctx, a, b, "greater"), a, b, "sum");
    else if (mode == SUM)
        sum = anvil_build_add(ctx, total, a, "sum");
    else
        sum = anvil_build_fadd(ctx, anvil_build_fmul(ctx, a, k, "product"), b, "sum");
    if (mode != SUM)
        anvil_build_store(ctx, sum, c_address);
    anvil_value_t *next = anvil_build_add(ctx, index, anvil_const_i32(ctx, 1), "next");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(index, anvil_const_i32(ctx, 0), func->entry);
    anvil_phi_add_incoming(index, next, body);
    if (total) {
        anvil_phi_add_incoming(total, anvil_const_i32(ctx, 5), func->entry);
        anvil_phi_add_incoming(total, sum, body);
    }

    anvil_set_insert_point(ctx, exit);
    if (total)
        anvil_build_store(ctx, total, anvil_func_get_param(func, 2));
    anvil_build_ret_void(ctx);

    if (allowance != SIZE_MAX)
        anvil_test_fail_alloc_after(ctx, allowance);
    anvil_pass_result_t status = anvil_pass_loop_vectorize(func);
    anvil_test_disable_alloc_fail(ctx);
    anvil_ctx_clear_error(ctx);

    char error[256] = {0};
    bool valid = anvil_func_verify(func, error, sizeof(error));
    *completed = status != ANVIL_PASS_RUN_ERROR;
    if (*completed) {
        valid &= status == (vectorizes(mode) ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED);
        /* A second run leaves the vector loop and the scalar remainder alone. */
        valid &= anvil_pass_loop_vectorize(func) == ANVIL_PASS_RUN_UNCHANGED;
        char *assembly = NULL;
        size_t length = 0;
        valid &= anvil_module_codegen(module, &assembly, &length) == ANVIL_OK;
        if (vectorizes(mode))
            valid &= assembly && strstr(assembly, target->expected[mode][0]) && strstr(assembly, target->expected[mode][1]);
        free(assembly);
    }

    if (!valid) {
        fprintf(stderr, "loop vectorize failure: arch=%d mode=%u allowance=%zu status=%d error=%s context=%s\n", (int)target->arch, mode,
                allowance, status, error, anvil_ctx_get_error(ctx));
        anvil_dump_module(stderr, module);
    }
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

/* for (i = 0; i < n; i++) if (a[i] == k) return i; return -1;  A search
 * loop: the vector loop tests a whole vector for the exit, and the scalar
 * loop finds the element. */
static const char *const search_expected[][2] = {
    {"pcmpeqd", "pmovmskb"}, {"cmeq v", "umaxv"}, {"vcmpequw", "sradi"}, {"vcmpequw", "sradi"},
};

static bool run_search_case(size_t target_index, size_t allowance, bool *completed)
{
    const target_case_t *target = &targets[target_index];
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(target->arch);
    if (target->cpu)
        anvil_ctx_set_cpu(ctx, target->cpu);
    anvil_module_t *module = anvil_module_create(ctx, "search");
    anvil_type_t *element = anvil_type_i32(ctx);
    anvil_type_t *params[] = {anvil_type_ptr(ctx, element), element, element};
    anvil_func_t *func = anvil_func_create(module, "search", anvil_type_func(ctx, element, params, 3, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(func, "header");
    anvil_block_t *body = anvil_block_create(func, "body");
    anvil_block_t *latch = anvil_block_create(func, "latch");
    anvil_block_t *found = anvil_block_create(func, "found");
    anvil_block_t *missing = anvil_block_create(func, "missing");

    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *index = anvil_build_phi(ctx, element, "i");
    anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, index, anvil_func_get_param(func, 2), "more"), body, missing);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *address = anvil_build_gep(ctx, element, anvil_func_get_param(func, 0), &index, 1, "address");
    anvil_value_t *value = anvil_build_load(ctx, element, address, "value");
    anvil_build_br_cond(ctx, anvil_build_cmp_eq(ctx, value, anvil_func_get_param(func, 1), "hit"), found, latch);
    anvil_set_insert_point(ctx, latch);
    anvil_value_t *next = anvil_build_add(ctx, index, anvil_const_i32(ctx, 1), "next");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(index, anvil_const_i32(ctx, 0), func->entry);
    anvil_phi_add_incoming(index, next, latch);
    anvil_set_insert_point(ctx, found);
    anvil_build_ret(ctx, index);
    anvil_set_insert_point(ctx, missing);
    anvil_build_ret(ctx, anvil_const_i32(ctx, -1));

    if (allowance != SIZE_MAX)
        anvil_test_fail_alloc_after(ctx, allowance);
    anvil_pass_result_t status = anvil_pass_loop_vectorize(func);
    anvil_test_disable_alloc_fail(ctx);
    anvil_ctx_clear_error(ctx);

    char error[256] = {0};
    bool valid = anvil_func_verify(func, error, sizeof(error));
    *completed = status != ANVIL_PASS_RUN_ERROR;
    if (*completed) {
        valid &= status == ANVIL_PASS_RUN_CHANGED;
        char *assembly = NULL;
        size_t length = 0;
        valid &= anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly &&
                 strstr(assembly, search_expected[target_index][0]) && strstr(assembly, search_expected[target_index][1]);
        free(assembly);
    }
    if (!valid) {
        fprintf(stderr, "search vectorize failure: arch=%d allowance=%zu status=%d error=%s\n", (int)target->arch, allowance, status, error);
        anvil_dump_module(stderr, module);
    }
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

/* int s = 5; for (i = 0; i < n; i++) s += ext(a[i]);  A sum of narrow
 * elements into a wider accumulator accumulates the lanes into wider ones
 * with the target's widening form (or stays scalar where it has none). */
typedef struct {
    anvil_arch_t arch;
    anvil_cpu_model_t cpu;
    bool sign;
    unsigned element_bits, accumulator_bits;
    const char *expected;   /* NULL: stays scalar */
} accumulate_case_t;

static const accumulate_case_t accumulate_cases[] = {
    {ANVIL_ARCH_X86_64, 0, false, 8, 32, "psadbw"},
    {ANVIL_ARCH_X86_64, 0, false, 8, 64, "paddq"},
    {ANVIL_ARCH_X86_64, 0, true, 16, 32, "pmaddwd"},
    {ANVIL_ARCH_X86_64, 0, false, 16, 32, "psrld"},
    {ANVIL_ARCH_X86_64, 0, true, 8, 32, NULL},           /* pmaddubsw needs SSSE3 */
    {ANVIL_ARCH_X86_64, ANVIL_CPU_X86_64_HASWELL, true, 8, 32, "pmaddubsw"},
    {ANVIL_ARCH_ARM64, 0, false, 8, 32, "uadalp v"},
    {ANVIL_ARCH_ARM64, 0, true, 16, 64, "sadalp v"},
    {ANVIL_ARCH_PPC64, ANVIL_CPU_PPC64_POWER8, false, 8, 32, "vsum4ubs"},
    {ANVIL_ARCH_PPC64LE, ANVIL_CPU_PPC64_POWER8, true, 16, 32, "vsum4shs"},
    {ANVIL_ARCH_PPC64LE, ANVIL_CPU_PPC64_POWER8, false, 16, 32, NULL},
};

static anvil_type_t *integer_type(anvil_ctx_t *ctx, unsigned bits, bool sign)
{
    switch (bits) {
    case 8: return sign ? anvil_type_i8(ctx) : anvil_type_u8(ctx);
    case 16: return sign ? anvil_type_i16(ctx) : anvil_type_u16(ctx);
    case 32: return sign ? anvil_type_i32(ctx) : anvil_type_u32(ctx);
    default: return sign ? anvil_type_i64(ctx) : anvil_type_u64(ctx);
    }
}

static bool run_accumulate_case(const accumulate_case_t *test, size_t allowance, bool *completed)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(test->arch);
    if (test->cpu)
        anvil_ctx_set_cpu(ctx, test->cpu);
    anvil_module_t *module = anvil_module_create(ctx, "accumulate");
    anvil_type_t *element = integer_type(ctx, test->element_bits, test->sign);
    anvil_type_t *wide = integer_type(ctx, test->accumulator_bits, true), *index_type = anvil_type_i32(ctx);
    anvil_type_t *params[] = {anvil_type_ptr(ctx, element), index_type};
    anvil_func_t *func = anvil_func_create(module, "sum", anvil_type_func(ctx, wide, params, 2, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *index = anvil_build_phi(ctx, index_type, "i"), *total = anvil_build_phi(ctx, wide, "total");
    anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, index, anvil_func_get_param(func, 1), "more"), body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *address = anvil_build_gep(ctx, element, anvil_func_get_param(func, 0), &index, 1, "address");
    anvil_value_t *lane = anvil_build_load(ctx, element, address, "lane");
    anvil_value_t *widened = test->sign ? anvil_build_sext(ctx, lane, wide, "widened") : anvil_build_zext(ctx, lane, wide, "widened");
    anvil_value_t *sum = anvil_build_add(ctx, total, widened, "sum");
    anvil_value_t *next = anvil_build_add(ctx, index, anvil_const_i32(ctx, 1), "next");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(index, anvil_const_i32(ctx, 0), func->entry);
    anvil_phi_add_incoming(index, next, body);
    anvil_phi_add_incoming(total, test->accumulator_bits == 64 ? anvil_const_i64(ctx, 5) : anvil_const_i32(ctx, 5), func->entry);
    anvil_phi_add_incoming(total, sum, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_ret(ctx, total);

    if (allowance != SIZE_MAX)
        anvil_test_fail_alloc_after(ctx, allowance);
    anvil_pass_result_t status = anvil_pass_loop_vectorize(func);
    anvil_test_disable_alloc_fail(ctx);
    anvil_ctx_clear_error(ctx);
    char error[256] = {0};
    bool valid = anvil_func_verify(func, error, sizeof(error));
    *completed = status != ANVIL_PASS_RUN_ERROR;
    if (*completed) {
        valid &= status == (test->expected ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED);
        char *assembly = NULL;
        size_t length = 0;
        valid &= anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && (!test->expected || (assembly && strstr(assembly, test->expected)));
        free(assembly);
    }
    if (!valid) {
        fprintf(stderr, "accumulate failure: arch=%d bits=%u->%u sign=%d allowance=%zu status=%d error=%s\n", (int)test->arch, test->element_bits,
                test->accumulator_bits, test->sign, allowance, status, error);
        anvil_dump_module(stderr, module);
    }
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

/* c[i] = clamp(ext(a[i]) op ext(b[i])) to the lanes' range becomes one
 * saturating lane operation; a clamp to another bound is no saturation and
 * stays scalar. */
typedef struct {
    anvil_arch_t arch;
    anvil_cpu_model_t cpu;
    unsigned bits;
    bool sign, subtract;
    int64_t high_bound;     /* the upper clamp's constant (lanes' maximum, or another) */
    const char *expected;   /* NULL: stays scalar */
} saturate_case_t;

static const saturate_case_t saturate_cases[] = {
    {ANVIL_ARCH_X86_64, 0, 8, false, false, 255, "paddusb"},
    {ANVIL_ARCH_X86_64, 0, 16, false, true, 0, "psubusw"},
    {ANVIL_ARCH_X86_64, 0, 16, true, false, 32767, "paddsw"},
    {ANVIL_ARCH_X86_64, 0, 8, false, false, 254, NULL},
    {ANVIL_ARCH_ARM64, 0, 8, true, true, 127, "sqsub v"},
    {ANVIL_ARCH_ARM64, 0, 16, false, false, 65535, "uqadd v"},
    {ANVIL_ARCH_PPC64LE, ANVIL_CPU_PPC64_POWER8, 8, false, true, 0, "vsububs"},
    {ANVIL_ARCH_PPC64, ANVIL_CPU_PPC64_POWER8, 16, true, false, 32767, "vaddshs"},
};

static bool run_saturate_case(const saturate_case_t *test)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(test->arch);
    if (test->cpu)
        anvil_ctx_set_cpu(ctx, test->cpu);
    anvil_module_t *module = anvil_module_create(ctx, "saturate");
    anvil_type_t *element = integer_type(ctx, test->bits, test->sign), *wide = anvil_type_i32(ctx), *index_type = anvil_type_i32(ctx);
    anvil_type_t *pointer = anvil_type_ptr(ctx, element);
    anvil_type_t *params[] = {pointer, pointer, pointer, index_type};
    anvil_func_t *func = anvil_func_create(module, "clamp", anvil_type_func(ctx, anvil_type_void(ctx), params, 4, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *index = anvil_build_phi(ctx, index_type, "i");
    anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, index, anvil_func_get_param(func, 3), "more"), body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *lanes[2];
    for (size_t side = 0; side < 2; side++) {
        anvil_value_t *address = anvil_build_gep(ctx, element, anvil_func_get_param(func, (unsigned)side), &index, 1, "address");
        anvil_value_t *lane = anvil_build_load(ctx, element, address, "lane");
        lanes[side] = test->sign ? anvil_build_sext(ctx, lane, wide, "widened") : anvil_build_zext(ctx, lane, wide, "widened");
    }
    anvil_value_t *value = test->subtract ? anvil_build_sub(ctx, lanes[0], lanes[1], "difference") : anvil_build_add(ctx, lanes[0], lanes[1], "sum");
    int64_t low = test->sign ? -(INT64_C(1) << (test->bits - 1)) : 0;
    /* min(value, high), then max(value, low) where a bound is needed. */
    if (test->sign || !test->subtract) {
        anvil_value_t *high = anvil_const_i32(ctx, (int32_t)test->high_bound);
        value = anvil_build_select(ctx, anvil_build_cmp_gt(ctx, value, high, "above"), high, value, "high");
    }
    if (test->sign || test->subtract) {
        anvil_value_t *floor = anvil_const_i32(ctx, (int32_t)low);
        value = anvil_build_select(ctx, anvil_build_cmp_lt(ctx, value, floor, "below"), floor, value, "low");
    }
    anvil_value_t *address = anvil_build_gep(ctx, element, anvil_func_get_param(func, 2), &index, 1, "out");
    anvil_build_store(ctx, anvil_build_trunc(ctx, value, element, "narrow"), address);
    anvil_value_t *next = anvil_build_add(ctx, index, anvil_const_i32(ctx, 1), "next");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(index, anvil_const_i32(ctx, 0), func->entry);
    anvil_phi_add_incoming(index, next, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_ret_void(ctx);

    anvil_pass_result_t status = anvil_pass_loop_vectorize(func);
    char error[256] = {0};
    char *assembly = NULL;
    size_t length = 0;
    bool valid = anvil_func_verify(func, error, sizeof(error)) && status == (test->expected ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED) &&
                 anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && (!test->expected || (assembly && strstr(assembly, test->expected)));
    if (!valid) {
        fprintf(stderr, "saturate failure: arch=%d bits=%u sign=%d subtract=%d status=%d error=%s\n%s\n", (int)test->arch, test->bits, test->sign,
                test->subtract, status, error, assembly ? assembly : "");
        anvil_dump_module(stderr, module);
    }
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

/* x * y + z contracts into one FMA only with the policy and where the
 * target has it (x86-64: FMA3; ARM64 and PowerPC always). */
static bool run_contract_case(anvil_arch_t arch, anvil_cpu_model_t cpu, bool policy, const char *expected)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    if (cpu)
        anvil_ctx_set_cpu(ctx, cpu);
    anvil_module_t *module = anvil_module_create(ctx, "contract");
    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_type_t *params[] = {f64, f64, f64};
    anvil_func_t *func = anvil_func_create(module, "multiply_add", anvil_type_func(ctx, f64, params, 3, false), ANVIL_LINK_EXTERNAL);
    anvil_func_set_fp_contraction(func, policy);
    anvil_set_insert_point(ctx, func->entry);
    anvil_value_t *product = anvil_build_fmul(ctx, anvil_func_get_param(func, 0), anvil_func_get_param(func, 1), "product");
    anvil_build_ret(ctx, anvil_build_fadd(ctx, product, anvil_func_get_param(func, 2), "sum"));

    anvil_pass_result_t status = anvil_pass_strength_reduce(func);
    char *assembly = NULL;
    size_t length = 0;
    bool fused = func->entry->first && func->entry->first->op == ANVIL_OP_FMA;
    bool valid = anvil_func_verify(func, NULL, 0) && status != ANVIL_PASS_RUN_ERROR && fused == (expected != NULL) &&
                 anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && (!expected || (assembly && strstr(assembly, expected)));
    if (!valid)
        fprintf(stderr, "contract failure: arch=%d policy=%d expected=%s\n%s\n", (int)arch, policy, expected ? expected : "(none)",
                assembly ? assembly : "");
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

int main(void)
{
    for (size_t test = 0; test < sizeof(saturate_cases) / sizeof(saturate_cases[0]); test++) {
        if (!run_saturate_case(&saturate_cases[test]))
            return 1;
    }
    for (size_t test = 0; test < sizeof(accumulate_cases) / sizeof(accumulate_cases[0]); test++) {
        bool completed = false;
        for (size_t allowance = 0; allowance < 400 && !completed; allowance++) {
            if (!run_accumulate_case(&accumulate_cases[test], test == 0 ? allowance : SIZE_MAX, &completed))
                return 1;
        }
        if (!completed)
            return 1;
    }
    if (!run_contract_case(ANVIL_ARCH_X86_64, ANVIL_CPU_X86_64_HASWELL, true, "vfmadd231sd") ||
        !run_contract_case(ANVIL_ARCH_X86_64, 0, true, NULL) || !run_contract_case(ANVIL_ARCH_ARM64, 0, true, "fmadd d") ||
        !run_contract_case(ANVIL_ARCH_ARM64, 0, false, NULL) || !run_contract_case(ANVIL_ARCH_PPC64, 0, true, "fmadd %f"))
        return 1;
    for (size_t target = 0; target < sizeof(targets) / sizeof(targets[0]); target++) {
        bool completed = false;
        for (size_t allowance = 0; allowance < 500 && !completed; allowance++) {
            if (!run_search_case(target, allowance, &completed))
                return 1;
        }
        if (!completed)
            return 1;
    }
    for (size_t target = 0; target < sizeof(targets) / sizeof(targets[0]); target++) {
        bool completed = false;
        for (size_t allowance = 0; allowance < 400 && !completed; allowance++) {
            if (!run_case(&targets[target], VECTORIZED, allowance, &completed))
                return 1;
        }
        if (!completed)
            return 1;
        for (unsigned mode = VECTORIZED; mode < MODE_COUNT; mode++) {
            if (!run_case(&targets[target], mode, SIZE_MAX, &completed) || !completed)
                return 1;
        }
    }
    puts("loop vectorize regression: PASS");
    return 0;
}
