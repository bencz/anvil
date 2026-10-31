/* Relocation models: which targets offer position independence, and how
 * each spells references to symbols that may live in another module. */
#include <anvil/anvil.h>
#include "platform/host.h"

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

typedef struct {
    anvil_arch_t arch;
    anvil_abi_t abi;
    const char *assembler_target;  /* NULL: no cross assembly */
    const char *got_load;          /* spelling of a GOT-relative access */
    const char *own_symbol;        /* this module's own internal global */
    const char *call_external;     /* how the external function is called */
} reloc_case_t;

/* int extern_value; static int own_value; int ext_fn(int);
 * int use(void) { return extern_value + own_value + ext_fn(1); } */
static char *build(const reloc_case_t *c, anvil_reloc_model_t model, anvil_error_t *set_result)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    char *text = NULL;
    if (!ctx || anvil_ctx_set_target(ctx, c->arch) != ANVIL_OK || anvil_ctx_set_abi(ctx, c->abi) != ANVIL_OK) {
        anvil_ctx_destroy(ctx);
        return NULL;
    }
    *set_result = anvil_ctx_set_reloc_model(ctx, model);
    if (*set_result == ANVIL_OK) {
        anvil_module_t *mod = anvil_module_create(ctx, "reloc");
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_value_t *ext = anvil_module_add_extern(mod, "extern_value", i32);
        anvil_value_t *own = anvil_module_add_global(mod, "own_value", i32, ANVIL_LINK_INTERNAL);
        anvil_global_set_initializer(own, anvil_const_i32(ctx, 3));
        anvil_type_t *params[] = { i32 };
        anvil_func_t *ext_fn = anvil_func_declare(mod, "ext_fn", anvil_type_func(ctx, i32, params, 1, false));
        anvil_func_t *use = anvil_func_create(mod, "use", anvil_type_func(ctx, i32, NULL, 0, false), ANVIL_LINK_EXTERNAL);
        anvil_set_insert_point(ctx, anvil_func_get_entry(use));
        anvil_value_t *zero = anvil_const_i32(ctx, 0);
        anvil_value_t *ext_ptr = anvil_build_gep(ctx, i32, ext, &zero, 1, "ext.ptr");
        anvil_value_t *own_ptr = anvil_build_gep(ctx, i32, own, &zero, 1, "own.ptr");
        anvil_value_t *arg = anvil_const_i32(ctx, 1), *called = NULL;
        anvil_build_call_checked(ctx, anvil_func_get_value(ext_fn), &arg, 1, "called", &called);
        anvil_value_t *sum = anvil_build_add(ctx, anvil_build_load(ctx, i32, ext_ptr, "e"), anvil_build_load(ctx, i32, own_ptr, "o"), "s");
        anvil_build_ret(ctx, anvil_build_add(ctx, sum, called, "r"));
        size_t len = 0;
        if (anvil_module_codegen(mod, &text, &len) != ANVIL_OK)
            text = NULL;
    }
    anvil_ctx_destroy(ctx);
    return text;
}

static void test_reloc_models(void)
{
    static const reloc_case_t cases[] = {
        { ANVIL_ARCH_X86_64, ANVIL_ABI_SYSV, NULL, "extern_value@GOTPCREL(%rip)", "own_value(%rip)", "call ext_fn@PLT" },
        { ANVIL_ARCH_X86, ANVIL_ABI_SYSV, NULL, "extern_value@GOT(", "own_value@GOTOFF(", "call *ext_fn@GOT(%eax)" },
        { ANVIL_ARCH_ARM64, ANVIL_ABI_SYSV, "aarch64-linux-gnu", ":got:extern_value", ":lo12:own_value", "bl ext_fn" },
        { ANVIL_ARCH_PPC64LE, ANVIL_ABI_SYSV, "powerpc64le-linux-gnu", "extern_value@got@ha", "own_value@toc@ha", "bl ext_fn" },
        { ANVIL_ARCH_PPC32, ANVIL_ABI_SYSV, "powerpc-linux-gnu", "extern_value@got(", "(own_value-.L", "ext_fn@got(%r12)" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const reloc_case_t *c = &cases[i];
        anvil_error_t set = ANVIL_OK;
        char *pic = build(c, ANVIL_RELOC_PIC, &set);
        CHECK(set == ANVIL_OK && pic, "ELF targets should produce shared-object code");
        if (pic) {
            CHECK(strstr(pic, c->got_load) != NULL, "a symbol another module defines is reached through the GOT");
            CHECK(strstr(pic, c->own_symbol) != NULL, "an internal symbol is reached directly, position-independently");
            CHECK(strstr(pic, c->call_external) != NULL, "an external function is called through the PLT or GOT");
            if (c->assembler_target)
                CHECK(anvil_test_host.cross_assemble(pic, c->assembler_target) <= 0, "position-independent code should assemble");
            free(pic);
        }
        char *fixed = build(c, ANVIL_RELOC_STATIC, &set);
        CHECK(fixed && strstr(fixed, "GOT") == NULL && strstr(fixed, ":got:") == NULL && strstr(fixed, "@got") == NULL,
              "static code does not use the GOT");
        free(fixed);
    }
}

static void test_model_selection(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    CHECK(anvil_ctx_set_reloc_model(ctx, ANVIL_RELOC_PIC) == ANVIL_ERR_NO_TARGET, "a target comes first");
    CHECK(anvil_ctx_set_target(ctx, ANVIL_ARCH_X86_64) == ANVIL_OK, "x86-64 should be selectable");
    CHECK(anvil_ctx_get_reloc_model(ctx) == ANVIL_RELOC_STATIC, "ELF defaults to static code");
    CHECK(anvil_ctx_set_abi(ctx, ANVIL_ABI_DARWIN) == ANVIL_OK && anvil_ctx_get_reloc_model(ctx) == ANVIL_RELOC_PIE,
          "Mach-O code is always position-independent");
    CHECK(anvil_ctx_set_abi(ctx, ANVIL_ABI_WIN64) == ANVIL_OK && anvil_ctx_set_reloc_model(ctx, ANVIL_RELOC_PIC) == ANVIL_ERR_INVALID_ARG,
          "Win64 has no GOT-based model");
    anvil_ctx_destroy(ctx);

    ctx = anvil_ctx_create();
    CHECK(anvil_ctx_set_target(ctx, ANVIL_ARCH_ZARCH) == ANVIL_OK &&
          anvil_ctx_set_reloc_model(ctx, ANVIL_RELOC_PIC) == ANVIL_ERR_INVALID_ARG &&
          anvil_ctx_set_reloc_model(ctx, ANVIL_RELOC_STATIC) == ANVIL_OK, "HLASM output stays static (the binder relocates)");
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    test_model_selection();
    test_reloc_models();
    if (failures) {
        fprintf(stderr, "%d relocation model check(s) failed\n", failures);
        return 1;
    }
    printf("relocation model regression tests passed\n");
    return 0;
}
