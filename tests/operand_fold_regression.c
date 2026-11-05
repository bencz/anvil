/* Instructions folded into the one reading them, on every target that has
 * the form: a load as an x86 memory operand, a shift as an ARM64 shifted
 * register operand, a shift and a mask as one PowerPC rotate-and-mask
 * (rlwinm, rldic*) or SystemZ RISBG (z10 on); SystemZ's distinct-operands
 * forms (z196 on) need no copy of the first operand; x86 stores constants
 * as immediates; vector loops with an accumulator run two body copies
 * per iteration, whose ARM64 accesses pair (ldp/stp). */
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

typedef enum {
    LOAD_ADD,         /* a + *p (32-bit) */
    SHIFT_ADD,        /* a + (b << 3) (64-bit) */
    SHIFT_SUB,        /* a - (b >> 2) (64-bit, arithmetic) */
    EXTRACT,          /* (x >> 4) & 15 (32-bit) */
    MASK_SCALE,       /* zext(x & 255) << 2 (32 then 64 bits) */
    KEEP_OPERAND,     /* (a + 5) ^ a (32-bit): the add cannot write a */
    STORE_CONSTANT,   /* *p = 7; return a (32-bit) */
} shape_t;

static anvil_func_t *build(anvil_ctx_t *ctx, anvil_module_t **module, shape_t shape)
{
    anvil_type_t *i32 = anvil_type_i32(ctx), *u32 = anvil_type_u32(ctx), *i64 = anvil_type_i64(ctx), *u64 = anvil_type_u64(ctx);
    *module = anvil_module_create(ctx, "fold");
    anvil_type_t *params[2];
    anvil_type_t *result;
    switch (shape) {
    case LOAD_ADD:
    case STORE_CONSTANT:
        params[0] = i32, params[1] = anvil_type_ptr(ctx, i32), result = i32;
        break;
    case SHIFT_ADD:
    case SHIFT_SUB:
        params[0] = i64, params[1] = i64, result = i64;
        break;
    case EXTRACT:
    case KEEP_OPERAND:
        params[0] = u32, params[1] = u32, result = u32;
        break;
    default:
        params[0] = u32, params[1] = u32, result = u64;
        break;
    }
    anvil_func_t *func = anvil_func_create(*module, "f", anvil_type_func(ctx, result, params, 2, false), ANVIL_LINK_EXTERNAL);
    anvil_value_t *a = anvil_func_get_param(func, 0), *b = anvil_func_get_param(func, 1), *value = NULL;
    anvil_set_insert_point(ctx, func->entry);
    switch (shape) {
    case LOAD_ADD:
        value = anvil_build_add(ctx, a, anvil_build_load(ctx, i32, b, "x"), "sum");
        break;
    case SHIFT_ADD:
        value = anvil_build_add(ctx, a, anvil_build_shl(ctx, b, anvil_const_i64(ctx, 3), "s"), "sum");
        break;
    case SHIFT_SUB:
        value = anvil_build_sub(ctx, a, anvil_build_sar(ctx, b, anvil_const_i64(ctx, 2), "s"), "diff");
        break;
    case EXTRACT:
        value = anvil_build_and(ctx, anvil_build_shr(ctx, a, anvil_const_u32(ctx, 4), "s"), anvil_const_u32(ctx, 15), "field");
        break;
    case KEEP_OPERAND:
        value = anvil_build_xor(ctx, anvil_build_add(ctx, a, anvil_const_u32(ctx, 5), "s"), a, "mixed");
        break;
    case STORE_CONSTANT:
        anvil_build_store(ctx, anvil_const_i32(ctx, 7), b);
        value = a;
        break;
    case MASK_SCALE: {
        anvil_value_t *low = anvil_build_and(ctx, anvil_build_xor(ctx, a, b, "x"), anvil_const_u32(ctx, 255), "low");
        value = anvil_build_shl(ctx, anvil_build_zext(ctx, low, u64, "wide"), anvil_const_u64(ctx, 2), "scaled");
        break;
    }
    }
    anvil_build_ret(ctx, value);
    return func;
}

typedef struct {
    anvil_arch_t arch;
    anvil_cpu_model_t cpu;
    shape_t shape;
    const char *line;      /* a line starting so ... */
    const char *operands;  /* ... and holding this */
    const char *absent;    /* no line starts so (NULL: unchecked) */
} fold_case_t;

static const fold_case_t cases[] = {
    {ANVIL_ARCH_X86_64, 0, LOAD_ADD, "\taddl (%", "), %e", "\tmovl (%"},
    {ANVIL_ARCH_X86, ANVIL_CPU_X86_PENTIUM4, LOAD_ADD, "\taddl ", "(%", NULL},
    {ANVIL_ARCH_ARM64, 0, SHIFT_ADD, "\tadd x", ", lsl #3", "\tlsl "},
    {ANVIL_ARCH_ARM64, 0, SHIFT_SUB, "\tsub x", ", asr #2", "\tasr "},
    {ANVIL_ARCH_PPC64LE, 0, EXTRACT, "\trlwinm ", ", 28, 28, 31", "\tsrwi "},
    {ANVIL_ARCH_PPC32, 0, EXTRACT, "\trlwinm ", ", 28, 28, 31", "\tandi. "},
    {ANVIL_ARCH_PPC64, 0, MASK_SCALE, "\trldic ", ", 2, 54", "\tsldi "},
    {ANVIL_ARCH_ZARCH, ANVIL_CPU_ZARCH_Z10, EXTRACT, "         RISBG ", ",60,191,60", "         SRL "},
    {ANVIL_ARCH_ZARCH, ANVIL_CPU_ZARCH_Z10, MASK_SCALE, "         RISBG ", ",54,189,2", "         SLLG "},
    /* Before z10 no RISBG: the shift and the mask stay. */
    {ANVIL_ARCH_ZARCH, ANVIL_CPU_ZARCH_Z9, EXTRACT, "         SRL ", "4", "         RISBG "},
    /* Distinct operands (z196): no copy before the add. */
    {ANVIL_ARCH_ZARCH, ANVIL_CPU_ZARCH_Z196, KEEP_OPERAND, "         AHIK ", ",5", "         LR "},
    {ANVIL_ARCH_ZARCH, ANVIL_CPU_ZARCH_Z10, KEEP_OPERAND, "         AHI ", ",5", "         AHIK "},
    /* A stored constant is the store's immediate. */
    {ANVIL_ARCH_X86_64, 0, STORE_CONSTANT, "\tmovl $7, (%", NULL, "\tmovl $7, %"},
    {ANVIL_ARCH_X86, ANVIL_CPU_X86_PENTIUM4, STORE_CONSTANT, "\tmovl $7, (%", NULL, "\tmovl $7, %"},
};

static bool has_line(const char *text, const char *start, const char *holding)
{
    for (const char *line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if (strncmp(line, start, strlen(start)) != 0)
            continue;
        if (!holding)
            return true;
        for (size_t at = 0; at + strlen(holding) <= length; at++)
            if (strncmp(line + at, holding, strlen(holding)) == 0)
                return true;
    }
    return false;
}

static void test_folds(void)
{
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        const fold_case_t *test = &cases[c];
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(test->arch);
        if (test->cpu)
            anvil_ctx_set_cpu(ctx, test->cpu);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        anvil_module_t *module = NULL;
        build(ctx, &module, test->shape);
        char *assembly = NULL;
        size_t length = 0;
        bool generated = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
        bool ok = generated && has_line(assembly, test->line, test->operands) && (!test->absent || !has_line(assembly, test->absent, NULL));
        char message[160];
        snprintf(message, sizeof(message), "case %zu (arch %d, shape %d): `%s...%s` folded", c, (int)test->arch, (int)test->shape, test->line + 1,
                 test->operands ? test->operands : "");
        CHECK(ok, message);
        if (!ok && assembly)
            fprintf(stderr, "%s\n", assembly);
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* sum += a[i] over i32: the vector loop keeps two accumulators (PHIs of
 * the vector header besides the index). */
static void test_interleave(void)
{
    static const anvil_arch_t arches[] = {ANVIL_ARCH_X86_64, ANVIL_ARCH_X86, ANVIL_ARCH_ARM64, ANVIL_ARCH_PPC64LE};
    for (size_t t = 0; t < sizeof(arches) / sizeof(arches[0]); t++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(arches[t]);
        if (arches[t] == ANVIL_ARCH_X86)
            anvil_ctx_set_cpu(ctx, ANVIL_CPU_X86_PENTIUM4);
        if (arches[t] == ANVIL_ARCH_PPC64LE)
            anvil_ctx_set_cpu(ctx, ANVIL_CPU_PPC64_POWER8);
        anvil_module_t *module = anvil_module_create(ctx, "sum");
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = {anvil_type_ptr(ctx, i32), i32};
        anvil_func_t *func = anvil_func_create(module, "sum", anvil_type_func(ctx, i32, params, 2, false), ANVIL_LINK_EXTERNAL);
        anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
        anvil_set_insert_point(ctx, func->entry);
        anvil_build_br(ctx, header);
        anvil_set_insert_point(ctx, header);
        anvil_value_t *i = anvil_build_phi(ctx, i32, "i"), *total = anvil_build_phi(ctx, i32, "total");
        anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, i, anvil_func_get_param(func, 1), "more"), body, exit);
        anvil_set_insert_point(ctx, body);
        anvil_value_t *lane = anvil_build_load(ctx, i32, anvil_build_gep(ctx, i32, anvil_func_get_param(func, 0), &i, 1, "at"), "lane");
        anvil_value_t *sum = anvil_build_add(ctx, total, lane, "sum");
        anvil_value_t *next = anvil_build_add(ctx, i, anvil_const_i32(ctx, 1), "next");
        anvil_build_br(ctx, header);
        anvil_phi_add_incoming(i, anvil_const_i32(ctx, 0), func->entry);
        anvil_phi_add_incoming(i, next, body);
        anvil_phi_add_incoming(total, anvil_const_i32(ctx, 7), func->entry);
        anvil_phi_add_incoming(total, sum, body);
        anvil_set_insert_point(ctx, exit);
        anvil_build_ret(ctx, total);

        anvil_pass_result_t status = anvil_pass_loop_vectorize(func);
        size_t phis = 0;
        for (anvil_block_t *block = func->blocks; block; block = block->next)
            if (block->name && strncmp(block->name, "vec.header", 10) == 0)
                for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
                    phis += instr->op == ANVIL_OP_PHI;
        char error[256] = {0};
        bool valid = anvil_func_verify(func, error, sizeof(error));
        char message[128];
        snprintf(message, sizeof(message), "arch %d: the vector sum keeps two accumulators", (int)arches[t]);
        CHECK(status == ANVIL_PASS_RUN_CHANGED && valid && phis == 3, message);
        if (!valid)
            fprintf(stderr, "verifier: %s\n", error);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* c[i] += k * b[i] (32-bit lanes) on ARM64 at O2: the two body copies'
 * accesses of each array share one pointer and pair, the loads and the
 * store written back: ldp q, q, [p], #32 / stp q, q, [p], #32. */
static void test_paired_accesses(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_ARM64);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    anvil_module_t *module = anvil_module_create(ctx, "axpy");
    anvil_type_t *i32 = anvil_type_i32(ctx), *i64 = anvil_type_i64(ctx);
    anvil_type_t *params[] = {anvil_type_ptr(ctx, i32), anvil_type_ptr(ctx, i32), i32, i64};
    anvil_func_t *func = anvil_func_create(module, "axpy", anvil_type_func(ctx, anvil_type_void(ctx), params, 4, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *i = anvil_build_phi(ctx, i64, "i");
    anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, i, anvil_func_get_param(func, 3), "more"), body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *at_c = anvil_build_gep(ctx, i32, anvil_func_get_param(func, 0), &i, 1, "at_c");
    anvil_value_t *at_b = anvil_build_gep(ctx, i32, anvil_func_get_param(func, 1), &i, 1, "at_b");
    anvil_value_t *product = anvil_build_mul(ctx, anvil_func_get_param(func, 2), anvil_build_load(ctx, i32, at_b, "b"), "product");
    anvil_build_store(ctx, anvil_build_add(ctx, anvil_build_load(ctx, i32, at_c, "c"), product, "sum"), at_c);
    anvil_value_t *next = anvil_build_add(ctx, i, anvil_const_i64(ctx, 1), "next");
    anvil_set_no_wrap(next, true);
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(i, anvil_const_i64(ctx, 0), func->entry);
    anvil_phi_add_incoming(i, next, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_ret_void(ctx);
    char *assembly = NULL;
    size_t length = 0;
    bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
    CHECK(built && strstr(assembly, "\tldp q") && strstr(assembly, "], #32\n") && strstr(assembly, "\tstp q"),
          "ARM64: the body copies' vector accesses pair (ldp/stp, written back)");
    if (!(built && strstr(assembly, "\tldp q") && strstr(assembly, "\tstp q")) && assembly)
        fprintf(stderr, "%s\n", assembly);
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

/* f(n) = n < 2 ? n : f(n - 1) + n: a call and callee-saved registers but
 * no locals.  PowerPC addresses its saves from r1 (no r31 frame pointer);
 * ARM64 pushes its pair (no sub/mov of sp). */
static void test_frames(void)
{
    static const struct {
        anvil_arch_t arch;
        const char *absent;
    } cases[] = {
        {ANVIL_ARCH_PPC64LE, "%r31"},
        {ANVIL_ARCH_PPC64, "%r31"},
        {ANVIL_ARCH_PPC32, "%r31"},
        {ANVIL_ARCH_ARM64, "mov sp, x29"},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(cases[c].arch);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        anvil_module_t *module = anvil_module_create(ctx, "frames");
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = {i32};
        anvil_type_t *type = anvil_type_func(ctx, i32, params, 1, false);
        anvil_func_t *func = anvil_func_create(module, "f", type, ANVIL_LINK_EXTERNAL);
        anvil_block_t *recurse = anvil_block_create(func, "recurse"), *base = anvil_block_create(func, "base");
        anvil_value_t *n = anvil_func_get_param(func, 0);
        anvil_set_insert_point(ctx, func->entry);
        anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, n, anvil_const_i32(ctx, 2), "small"), base, recurse);
        anvil_set_insert_point(ctx, recurse);
        anvil_value_t *arg = anvil_build_sub(ctx, n, anvil_const_i32(ctx, 1), "arg");
        anvil_value_t *inner = NULL;
        bool called = anvil_build_call_checked(ctx, anvil_func_get_value(func), &arg, 1, "inner", &inner);
        anvil_build_ret(ctx, called ? anvil_build_add(ctx, inner, n, "sum") : n);
        anvil_set_insert_point(ctx, base);
        anvil_build_ret(ctx, n);
        char *assembly = NULL;
        size_t length = 0;
        bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
        char message[128];
        snprintf(message, sizeof(message), "arch %d: the frame needs no frame-pointer work (%s)", (int)cases[c].arch, cases[c].absent);
        CHECK(built && !strstr(assembly, cases[c].absent), message);
        if (built && strstr(assembly, cases[c].absent))
            fprintf(stderr, "%s\n", assembly);
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* (a < b) as a value on SystemZ: with load on condition (z196 on) no
 * branch, LOCGR picks 1; before it, a branch. */
/* static int f(int a, int b) called only directly takes the target's
 * local convention (i386: a in ECX, b in EDX, as fastcall); one whose
 * address is stored, or an external one, keeps the default (cdecl: the
 * arguments on the stack). */
static void test_local_convention(void)
{
    for (int variant = 0; variant < 3; variant++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        anvil_module_t *module = anvil_module_create(ctx, "local");
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *pair[] = {i32, i32}, *one[] = {i32};
        anvil_type_t *f_type = anvil_type_func(ctx, i32, pair, 2, false);
        anvil_func_t *f = anvil_func_create(module, "f", f_type, variant == 2 ? ANVIL_LINK_EXTERNAL : ANVIL_LINK_INTERNAL);
        anvil_set_insert_point(ctx, f->entry);
        anvil_value_t *difference = anvil_build_sub(ctx, anvil_func_get_param(f, 0), anvil_func_get_param(f, 1), "d");
        anvil_build_ret(ctx, anvil_build_mul(ctx, difference, anvil_func_get_param(f, 0), "m"));
        anvil_func_t *g = anvil_func_create(module, "g", anvil_type_func(ctx, i32, one, 1, false), ANVIL_LINK_EXTERNAL);
        anvil_set_insert_point(ctx, g->entry);
        anvil_value_t *args[] = {anvil_func_get_param(g, 0), anvil_const_i32(ctx, 3)};
        anvil_value_t *result = NULL;
        anvil_build_call_checked(ctx, anvil_func_get_value(f), args, 2, "r", &result);
        if (variant == 1) {
            anvil_value_t *slot = anvil_module_add_global(module, "slot", anvil_type_ptr(ctx, f_type), ANVIL_LINK_EXTERNAL);
            anvil_build_store(ctx, anvil_func_get_value(f), slot);
        }
        anvil_build_ret(ctx, result);
        char *assembly = NULL;
        size_t length = 0;
        bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
        const char *body = built ? strstr(assembly, "\nf:") : NULL;
        const char *end = body ? strstr(body, ".size") : NULL;
        bool stack_args = false;
        for (const char *at = body; at && end && (at = strstr(at, "(%esp)")) && at < end; at++)
            stack_args = true;
        bool local = variant == 0;
        bool ok = body && end && stack_args != local;
        char message[128];
        snprintf(message, sizeof(message), "i386 variant %d: the function %s its arguments in registers", variant, local ? "takes" : "does not take");
        CHECK(ok, message);
        if (!ok && assembly)
            fprintf(stderr, "%s\n", assembly);
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* A byte load tested against zero, through a zero extension and directly:
 * IC inserts only the byte, so before a full-register compare the rest is
 * cleared or masked (or the compare tests the byte alone); a model with
 * extending loads loads with LLGC. */
static void test_systemz_byte_zero_test(void)
{
    static const struct {
        anvil_arch_t arch;
        anvil_cpu_model_t cpu;
    } cases[] = {{ANVIL_ARCH_ZARCH, ANVIL_CPU_ZARCH_Z900}, {ANVIL_ARCH_ZARCH, ANVIL_CPU_ZARCH_Z10}, {ANVIL_ARCH_S390, ANVIL_CPU_GENERIC}};
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        for (int direct = 0; direct < 2; direct++) {
            anvil_ctx_t *ctx = anvil_ctx_create_for_target(cases[c].arch);
            if (cases[c].cpu != ANVIL_CPU_GENERIC)
                anvil_ctx_set_cpu(ctx, cases[c].cpu);
            anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
            anvil_module_t *module = anvil_module_create(ctx, "NONZERO");
            anvil_type_t *i32 = anvil_type_i32(ctx), *u8 = anvil_type_u8(ctx);
            anvil_type_t *params[] = {anvil_type_ptr(ctx, u8)};
            anvil_func_t *func = anvil_func_create(module, "NONZERO", anvil_type_func(ctx, i32, params, 1, false), ANVIL_LINK_EXTERNAL);
            anvil_set_insert_point(ctx, func->entry);
            anvil_value_t *byte = anvil_build_load(ctx, u8, anvil_func_get_param(func, 0), "byte");
            anvil_value_t *test = direct ? anvil_build_cmp_ne(ctx, byte, anvil_const_u8(ctx, 0), "test")
                                         : anvil_build_cmp_ne(ctx, anvil_build_zext(ctx, byte, i32, "wide"), anvil_const_i32(ctx, 0), "test");
            anvil_build_ret(ctx, anvil_build_zext(ctx, test, i32, "value"));
            char *assembly = NULL;
            size_t length = 0;
            bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
            const char *insert = built ? strstr(assembly, " IC    ") : NULL;
            bool ok = built && (insert ? strstr(assembly, " XR    ") || strstr(assembly, " XGR   ") || strstr(assembly, " N     ") ||
                                             strstr(assembly, " TMLL  ") || strstr(assembly, " CLM   ")
                                       : strstr(assembly, " LLGC  ") != NULL);
            char message[128];
            snprintf(message, sizeof(message), "SystemZ arch %d cpu %d %s: a byte's zero test sees the byte alone", (int)cases[c].arch,
                     (int)cases[c].cpu, direct ? "direct" : "extended");
            CHECK(ok, message);
            if (!ok && assembly)
                fprintf(stderr, "%s\n", assembly);
            free(assembly);
            anvil_module_destroy(module);
            anvil_ctx_destroy(ctx);
        }
    }
}

static void test_systemz_compare_value(void)
{
    static const struct {
        anvil_cpu_model_t cpu;
        bool locgr;
    } cases[] = {{ANVIL_CPU_ZARCH_Z196, true}, {ANVIL_CPU_ZARCH_Z10, false}};
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_ZARCH);
        anvil_ctx_set_cpu(ctx, cases[c].cpu);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        anvil_module_t *module = anvil_module_create(ctx, "LESS");
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = {i32, i32};
        anvil_func_t *func = anvil_func_create(module, "LESS", anvil_type_func(ctx, i32, params, 2, false), ANVIL_LINK_EXTERNAL);
        anvil_set_insert_point(ctx, func->entry);
        anvil_value_t *less = anvil_build_cmp_lt(ctx, anvil_func_get_param(func, 0), anvil_func_get_param(func, 1), "less");
        anvil_build_ret(ctx, anvil_build_zext(ctx, less, i32, "value"));
        char *assembly = NULL;
        size_t length = 0;
        bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
        bool locgr = built && strstr(assembly, " LOCGR ") != NULL;
        char message[128];
        snprintf(message, sizeof(message), "SystemZ cpu %d: a compare's value %s LOCGR", (int)cases[c].cpu, cases[c].locgr ? "uses" : "has no");
        CHECK(built && locgr == cases[c].locgr, message);
        if (!(built && locgr == cases[c].locgr) && assembly)
            fprintf(stderr, "%s\n", assembly);
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* void f(p, lo, hi) { if (lo >= hi) return; f(p, lo + 1, hi); *p = lo; }:
 * the guard runs before the frame (shrink-wrapping): the compare comes
 * before the first frame instruction and the early path returns bare. */
static void test_early_return_before_frame(void)
{
    static const struct {
        anvil_arch_t arch;
        const char *frame;
        const char *compare;
    } cases[] = {
        {ANVIL_ARCH_ARM64, "\tstp x29", "\tcmp "},
        {ANVIL_ARCH_X86_64, "\tpushq %rbp", "\tcmpl "},
        {ANVIL_ARCH_PPC64LE, "\tmflr", "\tcmpw "},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(cases[c].arch);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        anvil_module_t *module = anvil_module_create(ctx, "guard");
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = {anvil_type_ptr(ctx, i32), i32, i32};
        anvil_type_t *type = anvil_type_func(ctx, anvil_type_void(ctx), params, 3, false);
        anvil_func_t *func = anvil_func_create(module, "guarded", type, ANVIL_LINK_EXTERNAL);
        anvil_block_t *work = anvil_block_create(func, "work"), *done = anvil_block_create(func, "done");
        anvil_value_t *p = anvil_func_get_param(func, 0), *lo = anvil_func_get_param(func, 1), *hi = anvil_func_get_param(func, 2);
        anvil_set_insert_point(ctx, func->entry);
        anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, lo, hi, "more"), work, done);
        anvil_set_insert_point(ctx, work);
        anvil_value_t *args[] = {p, anvil_build_add(ctx, lo, anvil_const_i32(ctx, 1), "next"), hi};
        anvil_build_call_checked(ctx, anvil_func_get_value(func), args, 3, "", NULL);
        anvil_build_store(ctx, lo, p);
        anvil_build_ret_void(ctx);
        anvil_set_insert_point(ctx, done);
        anvil_build_ret_void(ctx);
        char *assembly = NULL;
        size_t length = 0;
        bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
        const char *compare = built ? strstr(assembly, cases[c].compare) : NULL, *frame = built ? strstr(assembly, cases[c].frame) : NULL;
        char message[128];
        snprintf(message, sizeof(message), "arch %d: the guard's compare runs before the frame", (int)cases[c].arch);
        CHECK(compare && frame && compare < frame, message);
        if (!(compare && frame && compare < frame) && assembly)
            fprintf(stderr, "%s\n", assembly);
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* if (x & 16) g(): the branch tests the bit without computing x & 16:
 * ARM64 tbz/tbnz #4, x86 test (keeps x), SystemZ test under mask. */
static void test_bit_branch(void)
{
    static const struct {
        anvil_arch_t arch;
        anvil_cpu_model_t cpu;
        const char *form, *other, *absent;
    } cases[] = {
        {ANVIL_ARCH_ARM64, ANVIL_CPU_GENERIC, "\ttbz w0, #4, ", "\ttbnz w0, #4, ", "\tand "},
        {ANVIL_ARCH_X86_64, ANVIL_CPU_GENERIC, "\ttestl $16, ", NULL, "\tandl "},
        {ANVIL_ARCH_X86, ANVIL_CPU_GENERIC, "\ttestl $16, ", NULL, "\tandl "},
        {ANVIL_ARCH_ZARCH, ANVIL_CPU_ZARCH_Z196, " TMLL ", NULL, " NILF "},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(cases[c].arch);
        if (cases[c].cpu != ANVIL_CPU_GENERIC)
            anvil_ctx_set_cpu(ctx, cases[c].cpu);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        anvil_module_t *module = anvil_module_create(ctx, "BITS");
        anvil_type_t *u32 = anvil_type_u32(ctx), *void_type = anvil_type_void(ctx);
        anvil_func_t *callee = anvil_func_declare(module, "G", anvil_type_func(ctx, void_type, NULL, 0, false));
        anvil_type_t *params[] = {u32};
        anvil_func_t *func = anvil_func_create(module, "F", anvil_type_func(ctx, void_type, params, 1, false), ANVIL_LINK_EXTERNAL);
        anvil_block_t *call = anvil_block_create(func, "call"), *done = anvil_block_create(func, "done");
        anvil_set_insert_point(ctx, func->entry);
        anvil_value_t *bit = anvil_build_and(ctx, anvil_func_get_param(func, 0), anvil_const_u32(ctx, 16), "bit");
        anvil_build_br_cond(ctx, anvil_build_cmp_ne(ctx, bit, anvil_const_u32(ctx, 0), "set"), call, done);
        anvil_set_insert_point(ctx, call);
        anvil_build_call_checked(ctx, anvil_func_get_value(callee), NULL, 0, "", NULL);
        anvil_build_br(ctx, done);
        anvil_set_insert_point(ctx, done);
        anvil_build_ret_void(ctx);
        char *assembly = NULL;
        size_t length = 0;
        bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
        bool ok = built && (strstr(assembly, cases[c].form) || (cases[c].other && strstr(assembly, cases[c].other))) && !strstr(assembly, cases[c].absent);
        char message[128];
        snprintf(message, sizeof(message), "arch %d: a one-bit test branches on the bit (%s)", (int)cases[c].arch, cases[c].form + 1);
        CHECK(ok, message);
        if (!ok && assembly)
            fprintf(stderr, "%s\n", assembly);
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* c + (a u< b) adds the condition itself: ARM64 cinc, x86 adc $0 on the
 * compare's carry (no setcc/cset and extension). */
static void test_condition_add(void)
{
    static const struct {
        anvil_arch_t arch;
        const char *form;
        const char *absent;
    } cases[] = {
        {ANVIL_ARCH_ARM64, "\tcinc ", "\tcset "},
        {ANVIL_ARCH_X86_64, "\tadcl $0, ", "\tsetb "},
        {ANVIL_ARCH_X86, "\tadcl $0, ", "\tsetb "},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(cases[c].arch);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        anvil_module_t *module = anvil_module_create(ctx, "count");
        anvil_type_t *u32 = anvil_type_u32(ctx);
        anvil_type_t *params[] = {u32, u32, u32};
        anvil_func_t *func = anvil_func_create(module, "count", anvil_type_func(ctx, u32, params, 3, false), ANVIL_LINK_EXTERNAL);
        anvil_set_insert_point(ctx, func->entry);
        anvil_value_t *less = anvil_build_cmp_ult(ctx, anvil_func_get_param(func, 0), anvil_func_get_param(func, 1), "less");
        anvil_build_ret(ctx, anvil_build_add(ctx, anvil_func_get_param(func, 2), anvil_build_zext(ctx, less, u32, "one"), "sum"));
        char *assembly = NULL;
        size_t length = 0;
        bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
        bool ok = built && strstr(assembly, cases[c].form) && !strstr(assembly, cases[c].absent);
        char message[128];
        snprintf(message, sizeof(message), "arch %d: x + (a u< b) adds the condition (%s)", (int)cases[c].arch, cases[c].form + 1);
        CHECK(ok, message);
        if (!ok && assembly)
            fprintf(stderr, "%s\n", assembly);
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* if (x & 16) g() on PowerPC: andi. records CR0, the branch reads it (no
 * cmpwi), for a 32-bit value on a 64-bit target too. */
static void test_record_of_narrow_and(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_PPC64LE);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    anvil_module_t *module = anvil_module_create(ctx, "bits");
    anvil_type_t *u32 = anvil_type_u32(ctx), *void_type = anvil_type_void(ctx);
    anvil_func_t *callee = anvil_func_declare(module, "g", anvil_type_func(ctx, void_type, NULL, 0, false));
    anvil_type_t *params[] = {u32};
    anvil_func_t *func = anvil_func_create(module, "f", anvil_type_func(ctx, void_type, params, 1, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *call = anvil_block_create(func, "call"), *done = anvil_block_create(func, "done");
    anvil_set_insert_point(ctx, func->entry);
    anvil_value_t *bit = anvil_build_and(ctx, anvil_func_get_param(func, 0), anvil_const_u32(ctx, 16), "bit");
    anvil_build_br_cond(ctx, anvil_build_cmp_ne(ctx, bit, anvil_const_u32(ctx, 0), "set"), call, done);
    anvil_set_insert_point(ctx, call);
    anvil_build_call_checked(ctx, anvil_func_get_value(callee), NULL, 0, "", NULL);
    anvil_build_br(ctx, done);
    anvil_set_insert_point(ctx, done);
    anvil_build_ret_void(ctx);
    char *assembly = NULL;
    size_t length = 0;
    bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
    bool ok = built && strstr(assembly, "\tandi. ") && !strstr(assembly, "\tcmpwi ");
    CHECK(ok, "PowerPC: andi. answers the branch's test with zero");
    if (!ok && assembly)
        fprintf(stderr, "%s\n", assembly);
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

/* c >= 97 && c <= 122, as an i1 `and` or as select(t1, zext(t2), 0): one
 * unsigned compare of c - 97 with 25 after optimization. */
static void test_range_check(void)
{
    for (int form = 0; form < 2; form++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        anvil_module_t *module = anvil_module_create(ctx, "range");
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = {i32};
        anvil_func_t *func = anvil_func_create(module, "lower", anvil_type_func(ctx, i32, params, 1, false), ANVIL_LINK_EXTERNAL);
        anvil_set_insert_point(ctx, func->entry);
        anvil_value_t *c = anvil_func_get_param(func, 0);
        anvil_value_t *ge = anvil_build_cmp_ge(ctx, c, anvil_const_i32(ctx, 97), "ge"), *le = anvil_build_cmp_le(ctx, c, anvil_const_i32(ctx, 122), "le");
        anvil_value_t *result = form == 0 ? anvil_build_zext(ctx, anvil_build_and(ctx, ge, le, "both"), i32, "value")
                                          : anvil_build_select(ctx, ge, anvil_build_zext(ctx, le, i32, "wide"), anvil_const_i32(ctx, 0), "value");
        anvil_build_ret(ctx, result);
        bool optimized = anvil_module_optimize(module) == ANVIL_OK;
        size_t unsigned_compares = 0, signed_compares = 0;
        for (anvil_block_t *b = func->blocks; b; b = b->next)
            for (anvil_instr_t *instr = b->first; instr; instr = instr->next) {
                unsigned_compares += instr->op == ANVIL_OP_CMP_ULE && instr->operands[1]->kind == ANVIL_VAL_CONST_INT && instr->operands[1]->data.i == 25;
                signed_compares += instr->op == ANVIL_OP_CMP_GE || instr->op == ANVIL_OP_CMP_LE;
            }
        char message[96];
        snprintf(message, sizeof(message), "range check form %d: one unsigned compare (x - lo) u<= hi - lo", form);
        CHECK(optimized && unsigned_compares == 1 && signed_compares == 0, message);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

/* Compares of extensions compare the narrow values when the extension's
 * kind agrees with the order (any kind for eq/ne); a mixed one stays wide. */
static void test_narrow_extended_compare(void)
{
    static const struct {
        int op;           /* 0 eq, 1 lt (signed, i32), 2 ult (u32) */
        bool zero_extend; /* zext of u8 instead of sext of i8 */
        bool narrows;
    } cases[] = {
        {0, false, true}, {0, true, true}, {1, false, true}, {2, true, true}, {1, true, true}, {2, false, false},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        anvil_module_t *module = anvil_module_create(ctx, "narrow");
        anvil_type_t *i32 = anvil_type_i32(ctx), *wide_type = cases[index].op == 2 ? anvil_type_u32(ctx) : i32;
        anvil_type_t *narrow = cases[index].zero_extend ? anvil_type_u8(ctx) : anvil_type_i8(ctx);
        anvil_type_t *params[] = {narrow, narrow};
        anvil_func_t *func = anvil_func_create(module, "compare", anvil_type_func(ctx, i32, params, 2, false), ANVIL_LINK_EXTERNAL);
        anvil_set_insert_point(ctx, func->entry);
        anvil_value_t *wide[2];
        for (size_t side = 0; side < 2; side++) {
            anvil_value_t *param = anvil_func_get_param(func, side);
            wide[side] = cases[index].zero_extend ? anvil_build_zext(ctx, param, wide_type, "w") : anvil_build_sext(ctx, param, wide_type, "w");
        }
        anvil_value_t *cmp = cases[index].op == 0   ? anvil_build_cmp_eq(ctx, wide[0], wide[1], "c")
                             : cases[index].op == 1 ? anvil_build_cmp_lt(ctx, wide[0], wide[1], "c")
                                                    : anvil_build_cmp_ult(ctx, wide[0], wide[1], "c");
        anvil_build_ret(ctx, anvil_build_zext(ctx, cmp, i32, "r"));
        bool optimized = anvil_module_optimize(module) == ANVIL_OK;
        size_t narrow_compares = 0, wide_compares = 0;
        for (anvil_block_t *b = func->blocks; b; b = b->next)
            for (anvil_instr_t *instr = b->first; instr; instr = instr->next)
                if (instr->op >= ANVIL_OP_CMP_EQ && instr->op <= ANVIL_OP_CMP_UGE) {
                    bool is_narrow = anvil_type_bit_width(instr->operands[0]->type) == 8;
                    /* the signed order of zero-extended bytes is their unsigned order */
                    is_narrow &= !cases[index].zero_extend || cases[index].op != 1 || instr->op == ANVIL_OP_CMP_ULT;
                    narrow_compares += is_narrow;
                    wide_compares += !is_narrow;
                }
        char message[96];
        snprintf(message, sizeof(message), "extended compare case %zu: %s", index, cases[index].narrows ? "compares bytes" : "stays wide");
        CHECK(optimized && narrow_compares == (cases[index].narrows ? 1u : 0u) && wide_compares == (cases[index].narrows ? 0u : 1u), message);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
}

int main(void)
{
    test_local_convention();
    test_systemz_byte_zero_test();
    test_narrow_extended_compare();
    test_range_check();
    test_condition_add();
    test_record_of_narrow_and();
    test_bit_branch();
    test_early_return_before_frame();
    test_systemz_compare_value();
    test_frames();
    test_paired_accesses();
    test_folds();
    test_interleave();
    if (failures) {
        fprintf(stderr, "operand_fold_regression: %d failure(s)\n", failures);
        return 1;
    }
    printf("operand_fold_regression: all checks passed\n");
    return 0;
}
