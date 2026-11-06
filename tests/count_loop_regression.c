/* Counted loops whose index only addresses invariant bases count up to
 * zero (ANVIL_PASS_LOOP_STRENGTH), and every target branches on the
 * increment's own result: no compare is left in the loop. */
#include "anvil/anvil_internal.h"
#include "anvil/anvil_debug.h"
#include "anvil/anvil_opt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] %s\n", msg); \
        failures++; \
    } \
} while (0)

typedef enum { COPY_PLUS_ONE, INDEX_STORED, UNBOUNDED_STEP } loop_shape_t;

/* for (i = start; i < n; i++) out[i] = in[i] + 1   (COPY_PLUS_ONE)
 * the same storing i itself                       (INDEX_STORED: stays)
 * for (i = 0; i != n; i += 2) out[i] = in[i] + 1  (UNBOUNDED_STEP) */
static anvil_func_t *build_loop(anvil_ctx_t *ctx, anvil_module_t **module, loop_shape_t shape, int64_t start)
{
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(ctx);
    anvil_type_t *index = arch->ptr_size == 8 ? anvil_type_i64(ctx) : anvil_type_i32(ctx);
    anvil_type_t *element = index;
    *module = anvil_module_create(ctx, "count");
    anvil_type_t *params[] = {anvil_type_ptr(ctx, element), anvil_type_ptr(ctx, element), index};
    anvil_func_t *func = anvil_func_create(*module, "copy", anvil_type_func(ctx, anvil_type_void(ctx), params, 3, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
    anvil_value_t *out = anvil_func_get_param(func, 0), *in = anvil_func_get_param(func, 1), *n = anvil_func_get_param(func, 2);
    anvil_value_t *first = arch->ptr_size == 8 ? anvil_const_i64(ctx, start) : anvil_const_i32(ctx, (int32_t)start);
    anvil_value_t *step = arch->ptr_size == 8 ? anvil_const_i64(ctx, shape == UNBOUNDED_STEP ? 2 : 1)
                                              : anvil_const_i32(ctx, shape == UNBOUNDED_STEP ? 2 : 1);
    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *i = anvil_build_phi(ctx, index, "i");
    anvil_value_t *more = shape == UNBOUNDED_STEP ? anvil_build_cmp_ne(ctx, i, n, "more") : anvil_build_cmp_lt(ctx, i, n, "more");
    anvil_build_br_cond(ctx, more, body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *source = anvil_build_gep(ctx, element, in, &i, 1, "source");
    anvil_value_t *target = anvil_build_gep(ctx, element, out, &i, 1, "target");
    anvil_value_t *value = shape == INDEX_STORED ? i : anvil_build_add(ctx, anvil_build_load(ctx, element, source, "x"), step, "y");
    anvil_build_store(ctx, value, target);
    anvil_value_t *next = anvil_build_add(ctx, i, step, "next");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(i, first, func->entry);
    anvil_phi_add_incoming(i, next, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_ret_void(ctx);
    return func;
}

static anvil_instr_t *header_test(anvil_func_t *func)
{
    for (anvil_block_t *block = func->blocks; block; block = block->next)
        if (block->name && strcmp(block->name, "header") == 0 && block->last && block->last->op == ANVIL_OP_BR_COND)
            return block->last->operands[0]->data.instr;
    return NULL;
}

static void test_transform(void)
{
    for (loop_shape_t shape = COPY_PLUS_ONE; shape <= UNBOUNDED_STEP; shape++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
        anvil_module_t *module = NULL;
        anvil_func_t *func = build_loop(ctx, &module, shape, 3);
        anvil_pass_result_t status = anvil_pass_loop_strength(func);
        anvil_instr_t *test = header_test(func);
        char error[256] = {0};
        bool valid = anvil_func_verify(func, error, sizeof(error));
        bool counted = test && test->op == ANVIL_OP_CMP_NE && test->operands[1]->kind == ANVIL_VAL_CONST_INT && test->operands[1]->data.i == 0;
        char message[128];
        snprintf(message, sizeof(message), "shape %d %s count to zero", (int)shape, shape == INDEX_STORED ? "must not" : "must");
        CHECK(valid && counted == (shape != INDEX_STORED) && (status == ANVIL_PASS_RUN_CHANGED) == (shape != INDEX_STORED), message);
        if (counted && shape == COPY_PLUS_ONE) {
            /* The entry keeps i < n: the counter starts at 3 < n ? 3 - n : 0. */
            bool guarded = false;
            for (anvil_instr_t *instr = func->entry->first; instr; instr = instr->next)
                guarded |= instr->op == ANVIL_OP_SELECT;
            CHECK(guarded, "the counter's start keeps the original entry test");
        }
        if (!valid)
            fprintf(stderr, "verifier: %s\n", error);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

typedef struct {
    anvil_arch_t arch;
    anvil_cpu_model_t cpu;
    const char *increment;  /* the counter's increment ... */
    const char *branch;     /* ... and, on the next line, the branch */
} target_case_t;

static const target_case_t targets[] = {
    {ANVIL_ARCH_X86_64, 0, "\taddq $1, %", "\tjne "},
    {ANVIL_ARCH_X86, ANVIL_CPU_X86_PENTIUM4, "\taddl $1, %", "\tjne "},
    {ANVIL_ARCH_ARM64, 0, "\tadd x", "\tcbnz x"},
    /* PowerPC: the counter only counts once the streams took the
     * addresses: CTR (bdnz), update-form accesses (lwzu, stwu). */
    {ANVIL_ARCH_PPC64LE, 0, "\tstdu ", "\tbdnz "},
    {ANVIL_ARCH_PPC32, 0, "\tstwu ", "\tbdnz "},
};

/* Some line starting with `first` reaches one starting with `second`
 * within three lines, with no compare between (moves may be). */
static bool adjacent(const char *text, const char *first, const char *second)
{
    for (const char *line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (strncmp(line, first, strlen(first)) != 0)
            continue;
        const char *next = strchr(line, '\n');
        for (int step = 0; next && step < 3; step++, next = strchr(next + 1, '\n')) {
            const char *candidate = next + 1;
            if (strncmp(candidate, second, strlen(second)) == 0)
                return true;
            if (strncmp(candidate, "\tcmp", 4) == 0 || strncmp(candidate, "\ttest", 5) == 0)
                break;
        }
    }
    return false;
}

static void test_targets(void)
{
    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(targets[t].arch);
        if (targets[t].cpu)
            anvil_ctx_set_cpu(ctx, targets[t].cpu);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        anvil_module_t *module = NULL;
        build_loop(ctx, &module, COPY_PLUS_ONE, 0);
        char *assembly = NULL;
        size_t length = 0;
        bool generated = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
        char message[128];
        snprintf(message, sizeof(message), "arch %d branches on the counter's increment", (int)targets[t].arch);
        CHECK(generated && adjacent(assembly, targets[t].increment, targets[t].branch), message);
        if (generated && !adjacent(assembly, targets[t].increment, targets[t].branch))
            fprintf(stderr, "%s\n", assembly);
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* base = &p[16] (a constant offset, kept as base + 16 by the lowerers)
 * indexed in two sibling blocks: each block computes the address itself,
 * neither may use a register only the other block defined. */
static void test_constant_address_in_sibling_blocks(void)
{
    static const anvil_arch_t arches[] = {ANVIL_ARCH_X86_64, ANVIL_ARCH_X86, ANVIL_ARCH_ARM64, ANVIL_ARCH_PPC64LE, ANVIL_ARCH_PPC32, ANVIL_ARCH_ZARCH};
    for (size_t a = 0; a < sizeof(arches) / sizeof(arches[0]); a++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(arches[a]);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_NONE);
        anvil_module_t *module = anvil_module_create(ctx, "siblings");
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = {anvil_type_ptr(ctx, i32), i32, anvil_type_i1(ctx)};
        anvil_func_t *func = anvil_func_create(module, "pick", anvil_type_func(ctx, i32, params, 3, false), ANVIL_LINK_EXTERNAL);
        anvil_block_t *left = anvil_block_create(func, "left"), *right = anvil_block_create(func, "right");
        anvil_set_insert_point(ctx, func->entry);
        anvil_value_t *sixteen = anvil_const_i32(ctx, 16);
        anvil_value_t *base = anvil_build_gep(ctx, i32, anvil_func_get_param(func, 0), &sixteen, 1, "base");
        anvil_build_br_cond(ctx, anvil_func_get_param(func, 2), left, right);
        anvil_value_t *index = anvil_func_get_param(func, 1);
        anvil_set_insert_point(ctx, left);
        anvil_build_ret(ctx, anvil_build_load(ctx, i32, anvil_build_gep(ctx, i32, base, &index, 1, "l"), "lv"));
        anvil_set_insert_point(ctx, right);
        anvil_value_t *other = anvil_build_add(ctx, index, anvil_const_i32(ctx, 1), "other");
        anvil_build_ret(ctx, anvil_build_load(ctx, i32, anvil_build_gep(ctx, i32, base, &other, 1, "r"), "rv"));
        char *assembly = NULL;
        size_t length = 0;
        char message[96];
        snprintf(message, sizeof(message), "arch %d lowers a constant address used in sibling blocks", (int)arches[a]);
        CHECK(anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly, message);
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* for (u64 i = 0; i < n; i++) out[i] = 0 with a 4-byte element on
 * PowerPC: a pointer stream that starts one element early (update forms)
 * must not compute that start as an unsigned 0 - 1. */
static void test_unsigned_index_stream(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_PPC64);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    anvil_module_t *module = anvil_module_create(ctx, "unsigned_stream");
    anvil_type_t *u64 = anvil_type_u64(ctx), *u32 = anvil_type_u32(ctx);
    anvil_type_t *params[] = {anvil_type_ptr(ctx, u32), u64};
    anvil_func_t *func = anvil_func_create(module, "clear", anvil_type_func(ctx, anvil_type_void(ctx), params, 2, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *i = anvil_build_phi(ctx, u64, "i");
    anvil_build_br_cond(ctx, anvil_build_cmp_ult(ctx, i, anvil_func_get_param(func, 1), "more"), body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_build_store(ctx, anvil_const_u32(ctx, 0), anvil_build_gep(ctx, u32, anvil_func_get_param(func, 0), &i, 1, "slot"));
    anvil_value_t *next = anvil_build_add(ctx, i, anvil_const_u64(ctx, 1), "next");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(i, anvil_const_u64(ctx, 0), func->entry);
    anvil_phi_add_incoming(i, next, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_ret_void(ctx);
    char *assembly = NULL;
    size_t length = 0;
    bool ok = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly && strstr(assembly, "stwu ") && strstr(assembly, "bdnz ");
    CHECK(ok, "an unsigned-index stream on PowerPC compiles to update-form stores and a CTR loop");
    if (!ok)
        fprintf(stderr, "%s\n", assembly ? assembly : anvil_ctx_get_error(ctx));
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

/* for (i = 0; i < n; i++) s += 3 on z/Architecture: the counter only
 * counts, so the loop ends in BRCTG. */
static void test_systemz_branch_on_count(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_ZARCH);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    anvil_module_t *module = anvil_module_create(ctx, "brctg");
    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *params[] = {anvil_type_ptr(ctx, i64), i64};
    anvil_func_t *func = anvil_func_create(module, "steps", anvil_type_func(ctx, anvil_type_void(ctx), params, 2, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
    anvil_value_t *out = anvil_func_get_param(func, 0);
    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *i = anvil_build_phi(ctx, i64, "i"), *sum = anvil_build_phi(ctx, i64, "sum");
    anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, i, anvil_func_get_param(func, 1), "more"), body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *grown = anvil_build_add(ctx, anvil_build_mul(ctx, sum, anvil_const_i64(ctx, 3), "tripled"), anvil_const_i64(ctx, 1), "grown");
    anvil_value_t *next = anvil_build_add(ctx, i, anvil_const_i64(ctx, 1), "next");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(i, anvil_const_i64(ctx, 0), func->entry);
    anvil_phi_add_incoming(i, next, body);
    anvil_phi_add_incoming(sum, anvil_const_i64(ctx, 0), func->entry);
    anvil_phi_add_incoming(sum, grown, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_store(ctx, sum, out);
    anvil_build_ret_void(ctx);
    char *assembly = NULL;
    size_t length = 0;
    bool ok = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly && strstr(assembly, "BRCTG ");
    CHECK(ok, "a pure counter loop on z/Architecture ends in BRCTG");
    if (!ok)
        fprintf(stderr, "%s\n", assembly ? assembly : anvil_ctx_get_error(ctx));
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

/* while (i < n && v[i] < 100) i++;  as unsimplified code builds it: the
 * header's i < n branches to the second test or to the merge block, both
 * inside the loop, so a false i < n does not leave it by itself: no
 * counting to zero (the index's value at the exit is not n). */
static void test_header_test_must_leave_the_loop(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86);
    anvil_module_t *module = anvil_module_create(ctx, "and_merge");
    anvil_type_t *i32 = anvil_type_i32(ctx), *i16 = anvil_type_i16(ctx), *i1 = anvil_type_i1(ctx);
    anvil_type_t *params[] = {anvil_type_ptr(ctx, i16), i32};
    anvil_func_t *func = anvil_func_create(module, "run_below", anvil_type_func(ctx, i32, params, 2, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *cond = anvil_block_create(func, "cond"), *rhs = anvil_block_create(func, "rhs"), *merge = anvil_block_create(func, "merge"),
                  *body = anvil_block_create(func, "body"), *done = anvil_block_create(func, "done");
    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, cond);
    anvil_set_insert_point(ctx, cond);
    anvil_value_t *i = anvil_build_phi(ctx, i32, "i");
    anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, i, anvil_func_get_param(func, 1), "lt"), rhs, merge);
    anvil_set_insert_point(ctx, rhs);
    anvil_value_t *element = anvil_build_load(ctx, i16, anvil_build_gep(ctx, i16, anvil_func_get_param(func, 0), &i, 1, "at"), "v");
    anvil_value_t *small = anvil_build_cmp_lt(ctx, anvil_build_sext(ctx, element, i32, "w"), anvil_const_i32(ctx, 100), "small");
    anvil_build_br(ctx, merge);
    anvil_set_insert_point(ctx, merge);
    anvil_value_t *go = anvil_build_phi(ctx, i1, "go");
    anvil_phi_add_incoming(go, anvil_const_i1(ctx, false), cond);
    anvil_phi_add_incoming(go, small, rhs);
    anvil_build_br_cond(ctx, go, body, done);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *next = anvil_build_add(ctx, i, anvil_const_i32(ctx, 1), "next");
    anvil_build_br(ctx, cond);
    anvil_phi_add_incoming(i, anvil_const_i32(ctx, 0), func->entry);
    anvil_phi_add_incoming(i, next, body);
    anvil_set_insert_point(ctx, done);
    anvil_build_ret(ctx, i);
    anvil_pass_result_t status = anvil_pass_loop_strength(func);
    char error[256] = {0};
    CHECK(status == ANVIL_PASS_RUN_UNCHANGED && anvil_func_verify(func, error, sizeof(error)),
          "a header test whose false edge stays in the loop is no counted-loop test");
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

/* for (i = 0; i < n; i++) out[i] = (u64)in[i] (f32) on ppc64: the
 * conversion is printed as a helper call, which clobbers CTR: the loop
 * keeps its counter in a general register. */
static void test_helper_call_keeps_ctr_free(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_PPC64);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    anvil_module_t *module = anvil_module_create(ctx, "helper");
    anvil_type_t *i64 = anvil_type_i64(ctx), *u64 = anvil_type_u64(ctx), *f32 = anvil_type_f32(ctx);
    anvil_type_t *params[] = {anvil_type_ptr(ctx, u64), anvil_type_ptr(ctx, f32), i64};
    anvil_func_t *func = anvil_func_create(module, "convert", anvil_type_func(ctx, anvil_type_void(ctx), params, 3, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *i = anvil_build_phi(ctx, i64, "i");
    anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, i, anvil_func_get_param(func, 2), "more"), body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *x = anvil_build_load(ctx, f32, anvil_build_gep(ctx, f32, anvil_func_get_param(func, 1), &i, 1, "src"), "x");
    anvil_build_store(ctx, anvil_build_fptoui(ctx, x, u64, "y"), anvil_build_gep(ctx, u64, anvil_func_get_param(func, 0), &i, 1, "dst"));
    anvil_value_t *next = anvil_build_add(ctx, i, anvil_const_i64(ctx, 1), "next");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(i, anvil_const_i64(ctx, 0), func->entry);
    anvil_phi_add_incoming(i, next, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_ret_void(ctx);
    char *assembly = NULL;
    size_t length = 0;
    bool generated = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
    bool calls = generated && strstr(assembly, "\tbl ");
    CHECK(generated && (!calls || !strstr(assembly, "bdnz")), "a loop whose body calls a helper does not count in CTR");
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

/* while (s[i]) i++; return i: on targets whose accesses advance their base
 * the index lives on as a pointer (PowerPC lbzu, ARM64 pre-index), the
 * length is the pointer's distance; no constant is left in the loop. */
static void test_pointer_walk(void)
{
    static const struct {
        anvil_arch_t arch;
        const char *access;
        const char *absent;
    } walks[] = {
        {ANVIL_ARCH_PPC64LE, "\tlbzu ", "\tli "},
        {ANVIL_ARCH_PPC32, "\tlbzu ", "\tli "},
        {ANVIL_ARCH_ARM64, ", #1]!", "\tmov w"},
    };
    for (size_t w = 0; w < sizeof(walks) / sizeof(walks[0]); w++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(walks[w].arch);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(ctx);
        anvil_type_t *index = arch->ptr_size == 8 ? anvil_type_i64(ctx) : anvil_type_i32(ctx), *i8 = anvil_type_i8(ctx), *i32 = anvil_type_i32(ctx);
        anvil_module_t *module = anvil_module_create(ctx, "walk");
        anvil_type_t *params[] = {anvil_type_ptr(ctx, i8)};
        anvil_func_t *func = anvil_func_create(module, "length", anvil_type_func(ctx, i32, params, 1, false), ANVIL_LINK_EXTERNAL);
        anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
        anvil_set_insert_point(ctx, func->entry);
        anvil_build_br(ctx, header);
        anvil_set_insert_point(ctx, header);
        anvil_value_t *i = anvil_build_phi(ctx, index, "i");
        anvil_value_t *c = anvil_build_load(ctx, i8, anvil_build_gep(ctx, i8, anvil_func_get_param(func, 0), &i, 1, "at"), "c");
        anvil_build_br_cond(ctx, anvil_build_cmp_ne(ctx, c, anvil_const_i8(ctx, 0), "more"), body, exit);
        anvil_set_insert_point(ctx, body);
        anvil_value_t *one = arch->ptr_size == 8 ? anvil_const_i64(ctx, 1) : anvil_const_i32(ctx, 1);
        anvil_value_t *next = anvil_build_add(ctx, i, one, "next");
        anvil_set_no_wrap(next, true);
        anvil_build_br(ctx, header);
        anvil_phi_add_incoming(i, arch->ptr_size == 8 ? anvil_const_i64(ctx, 0) : anvil_const_i32(ctx, 0), func->entry);
        anvil_phi_add_incoming(i, next, body);
        anvil_set_insert_point(ctx, exit);
        anvil_build_ret(ctx, arch->ptr_size == 8 ? anvil_build_trunc(ctx, i, i32, "length") : i);
        char *assembly = NULL;
        size_t length = 0;
        bool ok = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly && strstr(assembly, walks[w].access);
        /* The loop: from its label to the branch back. */
        const char *loop = ok ? strstr(assembly, walks[w].access) : NULL;
        const char *start = loop;
        while (ok && start > assembly && !(start[0] == '\n' && start[1] == '.'))
            start--;
        const char *end = loop ? strstr(loop, "\n.L") : NULL;
        for (const char *line = start; ok && line && end && line < end; line = strchr(line + 1, '\n'))
            ok = strncmp(line + 1, walks[w].absent, strlen(walks[w].absent)) != 0;
        char message[128];
        snprintf(message, sizeof(message), "arch %d: the walk advances its pointer in the access, no constant in the loop", (int)walks[w].arch);
        CHECK(ok, message);
        if (!ok && assembly)
            fprintf(stderr, "%s\n", assembly);
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* The loop of `assembly` holding `marker`: from the label before it to the
 * next label (NULL when absent).  *end receives the stop. */
static const char *loop_around(const char *assembly, const char *marker, const char **end)
{
    const char *at = assembly ? strstr(assembly, marker) : NULL;
    if (!at)
        return NULL;
    const char *start = at;
    while (start > assembly && !(start[0] == '\n' && start[1] == '.'))
        start--;
    *end = strstr(at, "\n.L");
    if (!*end)
        *end = at + strlen(at);
    return start;
}

static bool loop_lacks(const char *start, const char *end, const char *text)
{
    for (const char *line = start; line && line < end; line = strchr(line + 1, '\n'))
        if (strncmp(line + 1, text, strlen(text)) == 0)
            return false;
    return true;
}

/* for (i = 0; i < n; i++) x ^= p[i] on a target with counter branches: the
 * byte index becomes a pointer stream so the counter is a pure counter
 * (PowerPC lbzu + bdnz, no index add or compare); signed bytes compared only
 * for equality load without a sign extension. */
static void test_counted_stream_frees_counter(void)
{
    static const anvil_arch_t arches[] = {ANVIL_ARCH_PPC64LE, ANVIL_ARCH_PPC64, ANVIL_ARCH_PPC32};
    for (size_t a = 0; a < sizeof(arches) / sizeof(arches[0]); a++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(arches[a]);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(ctx);
        anvil_type_t *index = arch->ptr_size == 8 ? anvil_type_u64(ctx) : anvil_type_u32(ctx), *i8 = anvil_type_i8(ctx), *i32 = anvil_type_i32(ctx);
        anvil_module_t *module = anvil_module_create(ctx, "stream");
        anvil_type_t *params[] = {anvil_type_ptr(ctx, i8), index, i8};
        anvil_func_t *func = anvil_func_create(module, "count_bytes", anvil_type_func(ctx, i32, params, 3, false), ANVIL_LINK_EXTERNAL);
        anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
        anvil_set_insert_point(ctx, func->entry);
        anvil_build_br(ctx, header);
        anvil_set_insert_point(ctx, header);
        anvil_value_t *i = anvil_build_phi(ctx, index, "i"), *total = anvil_build_phi(ctx, i32, "total");
        anvil_build_br_cond(ctx, anvil_build_cmp_ult(ctx, i, anvil_func_get_param(func, 1), "more"), body, exit);
        anvil_set_insert_point(ctx, body);
        anvil_value_t *c = anvil_build_load(ctx, i8, anvil_build_gep(ctx, i8, anvil_func_get_param(func, 0), &i, 1, "at"), "c");
        anvil_value_t *same = anvil_build_zext(ctx, anvil_build_cmp_eq(ctx, c, anvil_func_get_param(func, 2), "same"), i32, "hit");
        anvil_value_t *sum = anvil_build_add(ctx, total, same, "sum");
        anvil_value_t *next = anvil_build_add(ctx, i, arch->ptr_size == 8 ? anvil_const_u64(ctx, 1) : anvil_const_u32(ctx, 1), "next");
        anvil_build_br(ctx, header);
        anvil_phi_add_incoming(i, arch->ptr_size == 8 ? anvil_const_u64(ctx, 0) : anvil_const_u32(ctx, 0), func->entry);
        anvil_phi_add_incoming(i, next, body);
        anvil_phi_add_incoming(total, anvil_const_i32(ctx, 0), func->entry);
        anvil_phi_add_incoming(total, sum, body);
        anvil_set_insert_point(ctx, exit);
        anvil_build_ret(ctx, total);
        char *assembly = NULL;
        size_t length = 0;
        const char *end = NULL;
        bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK;
        /* The function's only loop: its text up to the end. */
        const char *loop = built ? strstr(assembly, "\tlbzu ") : NULL;
        end = loop ? loop + strlen(loop) : NULL;
        char message[160];
        snprintf(message, sizeof(message), "arch %d: the byte stream advances in lbzu and the loop ends in bdnz", (int)arches[a]);
        CHECK(loop && strstr(loop, "\tbdnz ") && loop_lacks(loop, end, "\taddic"), message);
        snprintf(message, sizeof(message), "arch %d: a byte compared only for equality loads without extsb", (int)arches[a]);
        CHECK(built && loop_lacks(assembly, assembly + strlen(assembly), "\textsb"), message);
        if (!(loop && strstr(loop, "\tbdnz ")) && assembly)
            fprintf(stderr, "%s\n", assembly);
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* for (j = s; j <= n; j += k) p[j] = 1 with a 32-bit j and an invariant
 * no-wrap step k: j widens (no extension per iteration) and the stored
 * constant leaves the loop. */
static void test_invariant_step_widens(void)
{
    static const struct {
        anvil_arch_t arch;
        const char *store, *extension, *constant;
    } cases[] = {
        {ANVIL_ARCH_PPC64LE, "\tstbx ", "\textsw", "\tli "},
        {ANVIL_ARCH_ARM64, "\tstrb ", "\tsxtw", "\tmov w"},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(cases[c].arch);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        anvil_type_t *i32 = anvil_type_i32(ctx), *u8 = anvil_type_u8(ctx), *i64 = anvil_type_i64(ctx);
        anvil_module_t *module = anvil_module_create(ctx, "multiples");
        anvil_type_t *params[] = {anvil_type_ptr(ctx, u8), i32, i32, i32};
        anvil_func_t *func = anvil_func_create(module, "mark", anvil_type_func(ctx, anvil_type_void(ctx), params, 4, false), ANVIL_LINK_EXTERNAL);
        anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
        anvil_set_insert_point(ctx, func->entry);
        anvil_build_br(ctx, header);
        anvil_set_insert_point(ctx, header);
        anvil_value_t *j = anvil_build_phi(ctx, i32, "j");
        anvil_build_br_cond(ctx, anvil_build_cmp_le(ctx, j, anvil_func_get_param(func, 2), "inside"), body, exit);
        anvil_set_insert_point(ctx, body);
        anvil_value_t *wide = anvil_build_sext(ctx, j, i64, "wide");
        anvil_build_store(ctx, anvil_const_u8(ctx, 1), anvil_build_gep(ctx, u8, anvil_func_get_param(func, 0), &wide, 1, "at"));
        anvil_value_t *next = anvil_build_add(ctx, j, anvil_func_get_param(func, 3), "next");
        anvil_set_no_wrap(next, true);
        anvil_build_br(ctx, header);
        anvil_phi_add_incoming(j, anvil_func_get_param(func, 1), func->entry);
        anvil_phi_add_incoming(j, next, body);
        anvil_set_insert_point(ctx, exit);
        anvil_build_ret_void(ctx);
        char *assembly = NULL;
        size_t length = 0;
        const char *end = NULL;
        bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK;
        const char *loop = built ? loop_around(assembly, cases[c].store, &end) : NULL;
        char message[160];
        snprintf(message, sizeof(message), "arch %d: the index widens and the stored constant is hoisted", (int)cases[c].arch);
        CHECK(loop && loop_lacks(loop, end, cases[c].extension) && loop_lacks(loop, end, cases[c].constant), message);
        if (!(loop && loop_lacks(loop, end, cases[c].extension) && loop_lacks(loop, end, cases[c].constant)) && assembly)
            fprintf(stderr, "%s\n", assembly);
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* for (i = 0, j = n - 1; i < j; i++, j--) swap s[i], s[j]: on a target with
 * counter branches the meeting indexes become a trip count (CTR) and two
 * pointer streams, one walking down (PowerPC lbzu 1(p) / lbzu -1(q),
 * bdnz). */
static void test_meeting_indexes(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_PPC64LE);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    anvil_type_t *i8 = anvil_type_i8(ctx), *i32 = anvil_type_i32(ctx), *i64 = anvil_type_i64(ctx);
    anvil_module_t *module = anvil_module_create(ctx, "reverse");
    anvil_type_t *params[] = {anvil_type_ptr(ctx, i8), i32};
    anvil_func_t *func = anvil_func_create(module, "reverse", anvil_type_func(ctx, anvil_type_void(ctx), params, 2, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
    anvil_set_insert_point(ctx, func->entry);
    anvil_value_t *last = anvil_build_sub(ctx, anvil_build_sext(ctx, anvil_func_get_param(func, 1), i64, "n"), anvil_const_i64(ctx, 1), "last");
    anvil_set_no_wrap(last, true);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *i = anvil_build_phi(ctx, i64, "i"), *j = anvil_build_phi(ctx, i64, "j");
    anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, i, j, "more"), body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *s = anvil_func_get_param(func, 0);
    anvil_value_t *at_i = anvil_build_gep(ctx, i8, s, &i, 1, "at_i"), *at_j = anvil_build_gep(ctx, i8, s, &j, 1, "at_j");
    anvil_value_t *x = anvil_build_load(ctx, i8, at_i, "x"), *y = anvil_build_load(ctx, i8, at_j, "y");
    anvil_build_store(ctx, y, at_i);
    anvil_build_store(ctx, x, at_j);
    anvil_value_t *up = anvil_build_add(ctx, i, anvil_const_i64(ctx, 1), "up"), *down = anvil_build_sub(ctx, j, anvil_const_i64(ctx, 1), "down");
    anvil_set_no_wrap(up, true);
    anvil_set_no_wrap(down, true);
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(i, anvil_const_i64(ctx, 0), func->entry);
    anvil_phi_add_incoming(i, up, body);
    anvil_phi_add_incoming(j, last, func->entry);
    anvil_phi_add_incoming(j, down, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_ret_void(ctx);
    char *assembly = NULL;
    size_t length = 0;
    bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
    bool ok = built && strstr(assembly, "\tbdnz ") && strstr(assembly, ", 1(%r") && strstr(assembly, ", -1(%r") && strstr(assembly, "\tlbzu ");
    CHECK(ok, "meeting indexes: a counted loop (bdnz) over two byte streams");
    if (!ok && assembly)
        fprintf(stderr, "%s\n", assembly);
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

/* x ^= p[i] for i < n on PowerPC (small_loop_unroll 4): four body copies
 * per test, the unrolled loop counting in CTR over one byte stream with
 * offsets (lbzu 4(p), lbz 1..3(p)), then the original loop for the rest. */
static void test_runtime_unroll(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_PPC64LE);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    anvil_type_t *u8 = anvil_type_u8(ctx), *u32 = anvil_type_u32(ctx), *u64 = anvil_type_u64(ctx);
    anvil_module_t *module = anvil_module_create(ctx, "fold");
    anvil_type_t *params[] = {anvil_type_ptr(ctx, u8), u64};
    anvil_func_t *func = anvil_func_create(module, "fold", anvil_type_func(ctx, u32, params, 2, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *i = anvil_build_phi(ctx, u64, "i"), *x = anvil_build_phi(ctx, u32, "x");
    anvil_build_br_cond(ctx, anvil_build_cmp_ult(ctx, i, anvil_func_get_param(func, 1), "more"), body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *byte = anvil_build_load(ctx, u8, anvil_build_gep(ctx, u8, anvil_func_get_param(func, 0), &i, 1, "at"), "byte");
    anvil_value_t *mixed = anvil_build_xor(ctx, anvil_build_shl(ctx, x, anvil_const_u32(ctx, 1), "shifted"), anvil_build_zext(ctx, byte, u32, "wide"), "mixed");
    anvil_value_t *next = anvil_build_add(ctx, i, anvil_const_u64(ctx, 1), "next");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(i, anvil_const_u64(ctx, 0), func->entry);
    anvil_phi_add_incoming(i, next, body);
    anvil_phi_add_incoming(x, anvil_const_u32(ctx, 0), func->entry);
    anvil_phi_add_incoming(x, mixed, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_ret(ctx, x);
    char *assembly = NULL;
    size_t length = 0;
    bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
    bool ok = built && strstr(assembly, "\tlbzu ") && strstr(assembly, ", 4(%r") && strstr(assembly, ", 3(%r") && strstr(assembly, "\tbdnz ");
    CHECK(ok, "runtime unrolling: four copies over one stream with offsets, counted in CTR");
    if (!ok && assembly)
        fprintf(stderr, "%s\n", assembly);
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

/* for (i = 0; i < 32; i++) t[i] = (i * 7) ^ (i >> 1): at O2 the loop is run
 * at compile time; t is filled from a read-only table holding exactly
 * those values. */
static void test_loop_table(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    anvil_type_t *u32 = anvil_type_u32(ctx), *u64 = anvil_type_u64(ctx);
    anvil_module_t *module = anvil_module_create(ctx, "tables");
    anvil_type_t *params[] = {anvil_type_ptr(ctx, u32)};
    anvil_func_t *func = anvil_func_create(module, "fill", anvil_type_func(ctx, anvil_type_void(ctx), params, 1, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *i = anvil_build_phi(ctx, u64, "i");
    anvil_build_br_cond(ctx, anvil_build_cmp_ult(ctx, i, anvil_const_u64(ctx, 32), "more"), body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *narrow = anvil_build_trunc(ctx, i, u32, "narrow");
    anvil_value_t *value = anvil_build_xor(ctx, anvil_build_mul(ctx, narrow, anvil_const_u32(ctx, 7), "times"),
                                           anvil_build_shr(ctx, narrow, anvil_const_u32(ctx, 1), "half"), "value");
    anvil_build_store(ctx, value, anvil_build_gep(ctx, u32, anvil_func_get_param(func, 0), &i, 1, "at"));
    anvil_value_t *next = anvil_build_add(ctx, i, anvil_const_u64(ctx, 1), "next");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(i, anvil_const_u64(ctx, 0), func->entry);
    anvil_phi_add_incoming(i, next, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_ret_void(ctx);
    bool optimized = anvil_module_optimize(module) == ANVIL_OK;
    const anvil_value_t *table = NULL;
    for (anvil_global_t *g = module->globals; g && !table; g = g->next)
        if (g->value->name && strstr(g->value->name, ".table."))
            table = g->value;
    const anvil_value_t *init = table ? table->data.global.init : NULL;
    bool ok = optimized && table && table->data.global.is_constant && init && init->data.aggregate.num_elements == 32;
    for (uint64_t k = 0; ok && k < 32; k++) {
        const anvil_value_t *element = init->data.aggregate.elements[k];
        ok = element->kind == ANVIL_VAL_CONST_INT && (uint32_t)element->data.u == (uint32_t)((k * 7) ^ (k >> 1));
    }
    CHECK(ok, "a loop storing constants per index becomes a read-only table of exactly those values");
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

/* do { c = *s; *d = c; s++; d++; } while (c): where the index folds into
 * the access and nothing advances its own base (x86), the two pointers
 * share one byte index (base + index accesses, one add, no copy back);
 * with update forms (PowerPC) they stay pointers. */
static void test_lockstep_pointers(void)
{
    static const struct {
        anvil_arch_t arch;
        const char *access; /* the loop's store */
        const char *add;
        size_t adds;
    } targets[] = {
        {ANVIL_ARCH_X86_64, ",1)\n", "\tadd", 1},
        {ANVIL_ARCH_X86, ",1)\n", "\tadd", 1},
        {ANVIL_ARCH_PPC64LE, "\tstbu ", "\taddi ", 0},
        {ANVIL_ARCH_ARM64, "\tstrb ", "\tadd ", 0},
    };
    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(targets[t].arch);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        anvil_type_t *i8 = anvil_type_i8(ctx), *p8 = anvil_type_ptr(ctx, i8);
        anvil_module_t *module = anvil_module_create(ctx, "lockstep");
        anvil_type_t *params[] = {p8, p8};
        anvil_func_t *func = anvil_func_create(module, "copy", anvil_type_func(ctx, anvil_type_void(ctx), params, 2, false), ANVIL_LINK_EXTERNAL);
        anvil_block_t *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
        anvil_set_insert_point(ctx, func->entry);
        anvil_build_br(ctx, body);
        anvil_set_insert_point(ctx, body);
        anvil_value_t *d = anvil_build_phi(ctx, p8, "d"), *s = anvil_build_phi(ctx, p8, "s");
        anvil_value_t *c = anvil_build_load(ctx, i8, s, "c");
        anvil_build_store(ctx, c, d);
        anvil_value_t *one = anvil_const_i32(ctx, 1);
        anvil_value_t *s_next = anvil_build_gep(ctx, i8, s, &one, 1, "s.next"), *d_next = anvil_build_gep(ctx, i8, d, &one, 1, "d.next");
        anvil_build_br_cond(ctx, anvil_build_cmp_ne(ctx, c, anvil_const_i8(ctx, 0), "more"), body, exit);
        anvil_phi_add_incoming(d, anvil_func_get_param(func, 0), func->entry);
        anvil_phi_add_incoming(d, d_next, body);
        anvil_phi_add_incoming(s, anvil_func_get_param(func, 1), func->entry);
        anvil_phi_add_incoming(s, s_next, body);
        anvil_set_insert_point(ctx, exit);
        anvil_build_ret_void(ctx);
        char *assembly = NULL;
        size_t length = 0;
        bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
        /* The last block with the access: the loop (a rotated copy of
         * its test may come first). */
        const char *end = NULL, *loop = built ? loop_around(assembly, targets[t].access, &end) : NULL;
        for (const char *later = loop ? strstr(end, targets[t].access) : NULL; later; later = strstr(end, targets[t].access))
            loop = loop_around(later, targets[t].access, &end);
        size_t adds = 0;
        for (const char *line = loop; line && line < end; line = strchr(line + 1, '\n'))
            adds += strncmp(line + 1, targets[t].add, strlen(targets[t].add)) == 0;
        bool ok = loop && adds == targets[t].adds && loop_lacks(loop, end, "\tmovq %") && loop_lacks(loop, end, "\tmovl %") &&
                  loop_lacks(loop, end, "\tmr ");
        char message[128];
        snprintf(message, sizeof(message), "arch %d: lockstep pointers cost one add per iteration", (int)targets[t].arch);
        CHECK(ok, message);
        if (!ok && assembly)
            fprintf(stderr, "%s\n", assembly);
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* for (int k = 1; k < n; k++) if (t[k]) { t[k - 1] += 3 * t[k]; t[k] = 0; }
 * with no-wrap int arithmetic: t[k - 1] shares t[k]'s index (the constant
 * leaves the GEP index for a base of its own), so the loop computes no
 * k - 1 and extends nothing; a stream reads t[k - 1] at t[k] - 4 where the
 * accesses advance their base, and the loop, branching inside, still counts
 * in the counter register where the target has one, the stream's advance
 * between the counter's step and its test. */
static void test_branching_counter_loop(void)
{
    static const struct {
        anvil_arch_t arch;
        anvil_cpu_model_t cpu;
        const char *present;
        const char *absent;
        const char *counter;
    } cases[] = {
        {ANVIL_ARCH_X86_64, ANVIL_CPU_GENERIC, NULL, "leal -1(", NULL},
        {ANVIL_ARCH_X86, ANVIL_CPU_GENERIC, NULL, "leal -1(", NULL},
        {ANVIL_ARCH_ARM64, ANVIL_CPU_GENERIC, NULL, "sxtw #2]", NULL},
        {ANVIL_ARCH_PPC64LE, ANVIL_CPU_GENERIC, "-4(%r", NULL, "\tbdnz "},
        {ANVIL_ARCH_PPC64, ANVIL_CPU_GENERIC, "-4(%r", NULL, "\tbdnz "},
        {ANVIL_ARCH_PPC32, ANVIL_CPU_GENERIC, "-4(%r", NULL, "\tbdnz "},
        /* No update forms to make the index a pure counter; t[k - 1]'s
         * base is computed once, before the loop. */
        {ANVIL_ARCH_ZARCH, ANVIL_CPU_ZARCH_Z196, NULL, NULL, NULL},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(cases[c].arch);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        if (cases[c].cpu != ANVIL_CPU_GENERIC)
            anvil_ctx_set_cpu(ctx, cases[c].cpu);
        anvil_module_t *module = anvil_module_create(ctx, "drain");
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = {anvil_type_ptr(ctx, i32), i32};
        anvil_func_t *func = anvil_func_create(module, "drain", anvil_type_func(ctx, anvil_type_void(ctx), params, 2, false), ANVIL_LINK_EXTERNAL);
        anvil_block_t *header = anvil_block_create(func, "header"), *test = anvil_block_create(func, "test");
        anvil_block_t *move = anvil_block_create(func, "move"), *latch = anvil_block_create(func, "latch");
        anvil_block_t *exit = anvil_block_create(func, "exit");
        anvil_value_t *tape = anvil_func_get_param(func, 0), *one = anvil_const_i32(ctx, 1);
        anvil_set_insert_point(ctx, func->entry);
        anvil_build_br(ctx, header);
        anvil_set_insert_point(ctx, header);
        anvil_value_t *k = anvil_build_phi(ctx, i32, "k");
        anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, k, anvil_func_get_param(func, 1), "more"), test, exit);
        anvil_set_insert_point(ctx, test);
        anvil_value_t *here = anvil_build_gep(ctx, i32, tape, &k, 1, "here");
        anvil_value_t *value = anvil_build_load(ctx, i32, here, "value");
        anvil_build_br_cond(ctx, anvil_build_cmp_ne(ctx, value, anvil_const_i32(ctx, 0), "full"), move, latch);
        anvil_set_insert_point(ctx, move);
        anvil_value_t *before = anvil_build_sub(ctx, k, one, "before");
        anvil_set_no_wrap(before, true);
        anvil_value_t *left = anvil_build_gep(ctx, i32, tape, &before, 1, "left");
        anvil_value_t *moved = anvil_build_add(ctx, anvil_build_load(ctx, i32, left, "neighbour"),
                                               anvil_build_mul(ctx, value, anvil_const_i32(ctx, 3), "tripled"), "moved");
        anvil_build_store(ctx, moved, left);
        anvil_build_store(ctx, anvil_const_i32(ctx, 0), here);
        anvil_build_br(ctx, latch);
        anvil_set_insert_point(ctx, latch);
        anvil_value_t *next = anvil_build_add(ctx, k, one, "next");
        anvil_set_no_wrap(next, true);
        anvil_build_br(ctx, header);
        anvil_phi_add_incoming(k, one, func->entry);
        anvil_phi_add_incoming(k, next, latch);
        anvil_set_insert_point(ctx, exit);
        anvil_build_ret_void(ctx);
        char *assembly = NULL;
        size_t length = 0;
        bool ok = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly && (!cases[c].present || strstr(assembly, cases[c].present)) &&
                  (!cases[c].absent || !strstr(assembly, cases[c].absent)) && (!cases[c].counter || strstr(assembly, cases[c].counter));
        char message[160];
        snprintf(message, sizeof(message), "arch %d: t[k - 1] shares t[k]'s index and the branching loop counts in its counter", (int)cases[c].arch);
        CHECK(ok, message);
        if (!ok)
            fprintf(stderr, "%s\n", assembly ? assembly : anvil_ctx_get_error(ctx));
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* for (uintptr_t i = 1; i < n; i++) s += a[i - 1] + a[i]: a[i - 1] is
 * a[i]'s address less one element, so on a pre-increment target the
 * stream's advance moves down to the first access with that derived
 * address (the IR stays valid; it once read the stream before its advance). */
static void test_stream_with_neighbour(void)
{
    static const anvil_arch_t arches[] = {ANVIL_ARCH_X86_64, ANVIL_ARCH_X86, ANVIL_ARCH_ARM64, ANVIL_ARCH_PPC64LE,
                                          ANVIL_ARCH_PPC64, ANVIL_ARCH_PPC32, ANVIL_ARCH_ZARCH};
    for (size_t a = 0; a < sizeof(arches) / sizeof(arches[0]); a++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(arches[a]);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(ctx);
        anvil_type_t *index = arch->ptr_size == 8 ? anvil_type_u64(ctx) : anvil_type_u32(ctx);
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_module_t *module = anvil_module_create(ctx, "neighbour");
        anvil_type_t *params[] = {anvil_type_ptr(ctx, i32), index};
        anvil_func_t *func = anvil_func_create(module, "neighbour", anvil_type_func(ctx, i32, params, 2, false), ANVIL_LINK_EXTERNAL);
        anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body");
        anvil_block_t *exit = anvil_block_create(func, "exit");
        anvil_value_t *base = anvil_func_get_param(func, 0), *one = arch->ptr_size == 8 ? anvil_const_u64(ctx, 1) : anvil_const_u32(ctx, 1);
        anvil_set_insert_point(ctx, func->entry);
        anvil_build_br(ctx, header);
        anvil_set_insert_point(ctx, header);
        anvil_value_t *i = anvil_build_phi(ctx, index, "i"), *sum = anvil_build_phi(ctx, i32, "sum");
        anvil_build_br_cond(ctx, anvil_build_cmp_ult(ctx, i, anvil_func_get_param(func, 1), "more"), body, exit);
        anvil_set_insert_point(ctx, body);
        anvil_value_t *before = anvil_build_sub(ctx, i, one, "before");
        anvil_value_t *left = anvil_build_load(ctx, i32, anvil_build_gep(ctx, i32, base, &before, 1, "left.at"), "left");
        anvil_value_t *here = anvil_build_load(ctx, i32, anvil_build_gep(ctx, i32, base, &i, 1, "here.at"), "here");
        anvil_value_t *grown = anvil_build_add(ctx, sum, anvil_build_add(ctx, left, here, "pair"), "grown");
        anvil_value_t *next = anvil_build_add(ctx, i, one, "next");
        anvil_build_br(ctx, header);
        anvil_phi_add_incoming(i, one, func->entry);
        anvil_phi_add_incoming(i, next, body);
        anvil_phi_add_incoming(sum, anvil_const_i32(ctx, 0), func->entry);
        anvil_phi_add_incoming(sum, grown, body);
        anvil_set_insert_point(ctx, exit);
        anvil_build_ret(ctx, sum);
        char *assembly = NULL;
        size_t length = 0;
        bool ok = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
        char message[128];
        snprintf(message, sizeof(message), "arch %d: a stream read at a[i - 1] and a[i] generates", (int)arches[a]);
        CHECK(ok, message);
        if (!ok)
            fprintf(stderr, "%s\n", anvil_ctx_get_error(ctx));
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* while (j != 0) { j++; s += 7; } with a 32- or 8-bit unsigned j: a
 * counter narrower than the counter register counts (-j) modulo 2^bits
 * (2^bits from 0), so PowerPC's CTR takes ((-j - 1) mod 2^bits) + 1, not a
 * sign-extended or wider count; SystemZ branches on 32 bits (BRCT), so an
 * 8-bit counter stays an ordinary loop there. */
static void test_narrow_counter_width(void)
{
    static const struct {
        anvil_arch_t arch;
        bool byte;
        const char *expect;
        const char *absent;
    } cases[] = {
        {ANVIL_ARCH_PPC64LE, false, "\trldicl %r0, %r0, 0, 32\n\taddic %r0, %r0, 1\n\tmtctr %r0", NULL},
        {ANVIL_ARCH_PPC64, false, "\trldicl %r0, %r0, 0, 32\n\taddic %r0, %r0, 1\n\tmtctr %r0", NULL},
        {ANVIL_ARCH_PPC32, false, "\tneg %r0, %r3\n\tmtctr %r0", NULL},
        {ANVIL_ARCH_PPC64LE, true, "\trldicl %r0, %r0, 0, 56\n\taddic %r0, %r0, 1\n\tmtctr %r0", NULL},
        {ANVIL_ARCH_PPC32, true, "\trlwinm %r0, %r0, 0, 24, 31\n\taddic %r0, %r0, 1\n\tmtctr %r0", NULL},
        {ANVIL_ARCH_ZARCH, false, "BRCT ", NULL},
        {ANVIL_ARCH_ZARCH, true, NULL, "BRCT"},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(cases[c].arch);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_BASIC);
        anvil_module_t *module = anvil_module_create(ctx, "wraps");
        anvil_type_t *u32 = cases[c].byte ? anvil_type_u8(ctx) : anvil_type_u32(ctx);
        anvil_type_t *params[] = {u32};
        anvil_func_t *func = anvil_func_create(module, "wraps", anvil_type_func(ctx, u32, params, 1, false), ANVIL_LINK_EXTERNAL);
        anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body");
        anvil_block_t *exit = anvil_block_create(func, "exit");
        anvil_value_t *start = anvil_func_get_param(func, 0);
        anvil_value_t *zero = cases[c].byte ? anvil_const_u8(ctx, 0) : anvil_const_u32(ctx, 0);
        anvil_value_t *one = cases[c].byte ? anvil_const_u8(ctx, 1) : anvil_const_u32(ctx, 1);
        anvil_value_t *seven = cases[c].byte ? anvil_const_u8(ctx, 7) : anvil_const_u32(ctx, 7);
        anvil_set_insert_point(ctx, func->entry);
        anvil_build_br(ctx, header);
        anvil_set_insert_point(ctx, header);
        anvil_value_t *j = anvil_build_phi(ctx, u32, "j"), *sum = anvil_build_phi(ctx, u32, "s");
        anvil_build_br_cond(ctx, anvil_build_cmp_ne(ctx, j, zero, "more"), body, exit);
        anvil_set_insert_point(ctx, body);
        anvil_value_t *next = anvil_build_add(ctx, j, one, "next");
        anvil_value_t *grown = anvil_build_add(ctx, sum, seven, "grown");
        anvil_build_br(ctx, header);
        anvil_phi_add_incoming(j, start, func->entry);
        anvil_phi_add_incoming(j, next, body);
        anvil_phi_add_incoming(sum, zero, func->entry);
        anvil_phi_add_incoming(sum, grown, body);
        anvil_set_insert_point(ctx, exit);
        anvil_build_ret(ctx, sum);
        char *assembly = NULL;
        size_t length = 0;
        /* An 8-bit counter tested through its extension stays an ordinary
         * loop; one counted in CTR must take the masked count. */
        bool ok = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
        bool counted = ok && (!cases[c].byte || strstr(assembly, "mtctr"));
        ok = ok && (!cases[c].expect || !counted || strstr(assembly, cases[c].expect)) && (!cases[c].absent || !strstr(assembly, cases[c].absent));
        char message[128];
        snprintf(message, sizeof(message), "arch %d: a %d-bit counter counts modulo its width", (int)cases[c].arch, cases[c].byte ? 8 : 32);
        CHECK(ok, message);
        if (!ok)
            fprintf(stderr, "%s\n", assembly ? assembly : anvil_ctx_get_error(ctx));
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

int main(void)
{
    test_stream_with_neighbour();
    test_branching_counter_loop();
    test_lockstep_pointers();
    test_loop_table();
    test_runtime_unroll();
    test_meeting_indexes();
    test_counted_stream_frees_counter();
    test_invariant_step_widens();
    test_pointer_walk();
    test_helper_call_keeps_ctr_free();
    test_header_test_must_leave_the_loop();
    test_unsigned_index_stream();
    test_systemz_branch_on_count();
    test_constant_address_in_sibling_blocks();
    test_transform();
    test_targets();
    test_narrow_counter_width();
    if (failures) {
        fprintf(stderr, "count_loop_regression: %d failure(s)\n", failures);
        return 1;
    }
    printf("count_loop_regression: all checks passed\n");
    return 0;
}
