/* Instruction selection across backends: constants become immediate
 * operands where the target can encode them, a multiplication by a power of
 * two becomes a shift, and a compare consumed by a branch branches on the
 * condition flags instead of materializing a boolean. */
#include <anvil/anvil.h>
#include <anvil/anvil_opt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/host.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "[FAIL] %s\n", (m)); failures++; } } while (0)

typedef struct {
    anvil_arch_t arch;
    const char *name;
    const char *cross_target; /* host cross-assembler target, NULL to skip */
    const char *present[5];
    const char *absent[2];
} isel_case_t;

static anvil_module_t *build_module(anvil_ctx_t *ctx)
{
    anvil_module_t *mod = anvil_module_create(ctx, "isel");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32 };
    anvil_func_t *fn = anvil_func_create(mod, "isel_pick", anvil_type_func(ctx, i32, params, 1, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *small = anvil_block_create(fn, "small");
    anvil_block_t *large = anvil_block_create(fn, "large");
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *x = anvil_func_get_param(fn, 0);
    anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, x, anvil_const_i32(ctx, 100), "less"), small, large);
    anvil_set_insert_point(ctx, small);
    anvil_value_t *scaled = anvil_build_mul(ctx, x, anvil_const_i32(ctx, 8), "scaled");
    anvil_build_ret(ctx, anvil_build_add(ctx, scaled, anvil_const_i32(ctx, 7), "sum"));
    anvil_set_insert_point(ctx, large);
    anvil_build_ret(ctx, anvil_build_and(ctx, x, anvil_const_i32(ctx, 0xff00), "masked"));
    return mod;
}

int main(void)
{
    static const isel_case_t cases[] = {
        { ANVIL_ARCH_X86_64, "x86-64", NULL, { "cmpl $100,", "shll $3,", "$7", "andl $65280,", "\tjge " }, { "setl", "movl $100," } },
        { ANVIL_ARCH_ARM64, "ARM64", "aarch64-linux-gnu", { "#100", "lsl w", "#7", "#0xff00", "\tb.ge " }, { "cset", "mov w9, #100" } },
        { ANVIL_ARCH_PPC64, "PPC64", "powerpc64-linux-gnu", { "cmpwi 0,", "slwi ", "addi ", "andi. ", "\tbge " }, { "cmp_true", "li %r0, 100" } },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_ctx_t *ctx = anvil_ctx_create();
        CHECK(ctx && anvil_ctx_set_target(ctx, cases[c].arch) == ANVIL_OK, "target should be available");
        if (!ctx)
            continue;
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        anvil_module_t *mod = build_module(ctx);
        char *text = NULL;
        size_t len = 0;
        CHECK(anvil_module_codegen(mod, &text, &len) == ANVIL_OK && text, "selection module should emit");
        if (text) {
            for (size_t p = 0; p < 5; p++) {
                if (!strstr(text, cases[c].present[p])) {
                    fprintf(stderr, "[FAIL] %s: expected '%s' in:\n%s\n", cases[c].name, cases[c].present[p], text);
                    failures++;
                }
            }
            for (size_t a = 0; a < 2; a++)
                CHECK(strstr(text, cases[c].absent[a]) == NULL, "a folded constant or boolean must not be materialized");
            if (cases[c].cross_target)
                CHECK(anvil_test_host.cross_assemble(text, cases[c].cross_target) <= 0, "selected code should assemble");
        }
        free(text);
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }
    if (failures) {
        fprintf(stderr, "%d instruction selection check(s) failed\n", failures);
        return 1;
    }
    printf("instruction selection regression checks passed (x86-64, ARM64, PPC64)\n");
    return 0;
}
