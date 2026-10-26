/* A global value addresses an object of its own type for load, store and
 * GEP; struct GEP must accept the same base on every target. */
#include <anvil/anvil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "[FAIL] %s\n", (m)); failures++; } } while (0)

static void check_target(anvil_arch_t arch, const char *name)
{
    char message[160];
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    CHECK(ctx != NULL, "context");
    if (!ctx) return;
    anvil_module_t *mod = anvil_module_create(ctx, "struct_gep_global");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *fields[] = { i32, i32 };
    anvil_type_t *record = anvil_type_literal_struct(ctx, fields, 2, false);
    anvil_value_t *global = anvil_module_add_global(mod, "pair", record,
                                                    ANVIL_LINK_EXTERNAL);
    anvil_value_t *init_fields[] = {
        anvil_const_i32(ctx, 3), anvil_const_i32(ctx, 4)
    };
    CHECK(global && anvil_global_set_initializer(
              global, anvil_const_struct(ctx, record, init_fields, 2)),
          "global initializer");

    anvil_type_t *fn_type = anvil_type_func(ctx, i32, &i32, 1, false);
    anvil_func_t *fn = anvil_func_create(mod, "read_pair", fn_type,
                                         ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));

    /* Rejected: a base that is neither a pointer nor a global. */
    CHECK(anvil_build_struct_gep(ctx, record, anvil_func_get_param(fn, 0),
                                 0, "bad") == NULL,
          "struct GEP must reject a non-address base");

    anvil_value_t *first = anvil_build_struct_gep(ctx, record, global, 0,
                                                  "first");
    anvil_value_t *second = anvil_build_struct_gep(ctx, record, global, 1,
                                                   "second");
    snprintf(message, sizeof(message),
             "%s: struct GEP accepts a global base", name);
    CHECK(first != NULL && second != NULL, message);
    if (first && second) {
        anvil_build_store(ctx, anvil_func_get_param(fn, 0), second);
        anvil_value_t *a = anvil_build_load(ctx, i32, first, "a");
        anvil_value_t *b = anvil_build_load(ctx, i32, second, "b");
        anvil_value_t *scaled = anvil_build_mul(ctx, a,
                                                anvil_const_i32(ctx, 10),
                                                "scaled");
        anvil_build_ret(ctx, anvil_build_add(ctx, scaled, b, "sum"));

        char error[256];
        snprintf(message, sizeof(message),
                 "%s: verifier accepts struct GEP of a global", name);
        CHECK(anvil_module_verify(mod, error, sizeof(error)), message);

        char *output = NULL;
        size_t length = 0;
        snprintf(message, sizeof(message), "%s: codegen", name);
        CHECK(anvil_module_codegen(mod, &output, &length) == ANVIL_OK &&
              output && length, message);
        free(output);
    }
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    static const struct { anvil_arch_t arch; const char *name; } targets[] = {
        { ANVIL_ARCH_X86, "x86" },
        { ANVIL_ARCH_X86_64, "x86_64" },
        { ANVIL_ARCH_S370, "s370" },
        { ANVIL_ARCH_S370_XA, "s370_xa" },
        { ANVIL_ARCH_S390, "s390" },
        { ANVIL_ARCH_ZARCH, "zarch" },
        { ANVIL_ARCH_PPC32, "ppc32" },
        { ANVIL_ARCH_PPC64, "ppc64" },
        { ANVIL_ARCH_PPC64LE, "ppc64le" },
        { ANVIL_ARCH_ARM64, "arm64" }
    };
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
        check_target(targets[i].arch, targets[i].name);
    if (failures) {
        fprintf(stderr, "struct GEP global regression: %d failure(s)\n",
                failures);
        return 1;
    }
    puts("struct GEP global regression: PASS");
    return 0;
}
