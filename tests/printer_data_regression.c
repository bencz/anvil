/* String literals a function uses are emitted by one shared routine
 * (backend/common/gnu_data.c) on every GNU-assembler target: quotes,
 * backslashes, control characters and bytes above 0x7e are escaped so the
 * assembler reads back the same bytes (the printers' own copies emitted
 * unprintable bytes raw). */
#include "anvil/anvil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg)                         \
    do {                                         \
        if (!(cond)) {                           \
            fprintf(stderr, "[FAIL] %s\n", msg); \
            failures++;                          \
        }                                        \
    } while (0)

static void test_target(anvil_arch_t arch, const char *name)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    anvil_module_t *module = anvil_module_create(ctx, "strings");
    anvil_type_t *ptr = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_func_t *func = anvil_func_create(module, "text", anvil_type_func(ctx, ptr, NULL, 0, false), ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(func));
    anvil_build_ret(ctx, anvil_const_string(ctx, "a\x01\x7f\x80\"\\\n"));
    char *assembly = NULL;
    size_t length = 0;
    char message[128];
    bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
    snprintf(message, sizeof(message), "%s: the literal's bytes are escaped (\\001 \\177 \\200, quote, backslash, newline)", name);
    CHECK(built && strstr(assembly, "\t.asciz \"a\\001\\177\\200\\\"\\\\\\n\"\n"), message);
    if (built && !strstr(assembly, "\t.asciz \"a\\001"))
        fprintf(stderr, "%s\n", assembly);
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    test_target(ANVIL_ARCH_X86_64, "x86-64");
    test_target(ANVIL_ARCH_X86, "i386");
    test_target(ANVIL_ARCH_ARM64, "ARM64");
    test_target(ANVIL_ARCH_PPC32, "ppc32");
    test_target(ANVIL_ARCH_PPC64LE, "ppc64le");
    if (failures) {
        fprintf(stderr, "%d printer data regression check(s) failed\n", failures);
        return 1;
    }
    printf("printer data regression tests passed\n");
    return 0;
}
