/* Narrow integers keep undefined high bits in registers: operations that
 * read them (signed division, compares, int->FP) must extend first on
 * targets without 8/16-bit forms, and int->FP everywhere. */
#include <anvil/anvil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/host.h"

static int failures;

static void expect(const char *text, const char *needle, const char *target)
{
    if (!text || !strstr(text, needle)) {
        fprintf(stderr, "[FAIL] %s: expected '%s'\n%s\n", target, needle, text ? text : "(no output)");
        failures++;
    }
}

int main(void)
{
    static const struct {
        anvil_arch_t arch;
        const char *name;
        const char *cross;
        const char *sign_extend;
        const char *convert_extend;
    } cases[] = {
        { ANVIL_ARCH_X86_64, "x86-64", NULL, "idivb", "movsbl" },
        { ANVIL_ARCH_ARM64, "ARM64", "aarch64-linux-gnu", "sxtb", "sxtb" },
        { ANVIL_ARCH_PPC64, "PPC64", "powerpc64-linux-gnu", "extsb", "extsb" },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_ctx_t *ctx = anvil_ctx_create();
        if (!ctx || anvil_ctx_set_target(ctx, cases[c].arch) != ANVIL_OK) {
            fprintf(stderr, "[FAIL] %s target unavailable\n", cases[c].name);
            failures++;
            anvil_ctx_destroy(ctx);
            continue;
        }
        anvil_module_t *mod = anvil_module_create(ctx, "narrow");
        anvil_type_t *i8 = anvil_type_i8(ctx);
        anvil_type_t *f64 = anvil_type_f64(ctx);
        anvil_type_t *params[] = { i8, i8 };

        anvil_func_t *div = anvil_func_create(mod, "div8", anvil_type_func(ctx, i8, params, 2, false), ANVIL_LINK_EXTERNAL);
        anvil_set_insert_point(ctx, anvil_func_get_entry(div));
        anvil_build_ret(ctx, anvil_build_sdiv(ctx, anvil_func_get_param(div, 0), anvil_func_get_param(div, 1), "q"));

        anvil_func_t *conv = anvil_func_create(mod, "conv8", anvil_type_func(ctx, f64, params, 1, false), ANVIL_LINK_EXTERNAL);
        anvil_set_insert_point(ctx, anvil_func_get_entry(conv));
        anvil_build_ret(ctx, anvil_build_sitofp(ctx, anvil_func_get_param(conv, 0), f64, "d"));

        char *text = NULL;
        size_t len = 0;
        if (anvil_module_codegen(mod, &text, &len) != ANVIL_OK || !text) {
            fprintf(stderr, "[FAIL] %s: codegen failed\n", cases[c].name);
            failures++;
        } else {
            const char *conv_body = strstr(text, "conv8:");
            expect(text, cases[c].sign_extend, cases[c].name);
            expect(conv_body, cases[c].convert_extend, cases[c].name);
            if (cases[c].cross && anvil_test_host.cross_assemble(text, cases[c].cross) > 0) {
                fprintf(stderr, "[FAIL] %s: output does not assemble\n", cases[c].name);
                failures++;
            }
        }
        free(text);
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }
    if (failures) {
        fprintf(stderr, "%d narrow integer check(s) failed\n", failures);
        return 1;
    }
    printf("narrow integer regression checks passed (x86-64, ARM64, PPC64)\n");
    return 0;
}
