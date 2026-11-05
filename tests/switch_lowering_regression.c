/* A switch of many cases lowers to sorted runs (consecutive values with
 * one destination tested as one unsigned range) decided by a balanced
 * tree of compares, on every MachineIR back end: here nine cases in five
 * runs take at most six compares, not nine. */
#include "anvil/anvil_internal.h"
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

static size_t count_lines(const char *text, const char *prefix)
{
    size_t count = 0, length = strlen(prefix);
    for (const char *line = text; line; line = strchr(line, '\n')) {
        if (*line == '\n')
            line++;
        if (strncmp(line, prefix, length) == 0)
            count++;
        if (!*line)
            break;
    }
    return count;
}

int main(void)
{
    static const struct {
        anvil_arch_t arch;
        const char *compare; /* NULL: lowering only */
    } targets[] = {
        {ANVIL_ARCH_X86_64, "\tcmp"}, {ANVIL_ARCH_X86, "\tcmp"}, {ANVIL_ARCH_ARM64, "\tcmp"},
        {ANVIL_ARCH_PPC64LE, "\tcmp"}, {ANVIL_ARCH_ZARCH, NULL},
    };
    static const struct {
        int value;
        int target;
    } cases[] = {{2, 0}, {0, 0}, {1, 0}, {3, 0}, {10, 1}, {21, 2}, {20, 2}, {100, 3}, {-5, 4}};
    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(targets[t].arch);
        anvil_module_t *module = anvil_module_create(ctx, "SWITCH");
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = {i32};
        anvil_func_t *func = anvil_func_create(module, "SWITCH", anvil_type_func(ctx, i32, params, 1, false), ANVIL_LINK_EXTERNAL);
        anvil_block_t *blocks[5], *otherwise = anvil_block_create(func, "otherwise");
        for (int b = 0; b < 5; b++) {
            char name[16];
            snprintf(name, sizeof(name), "dest%d", b);
            blocks[b] = anvil_block_create(func, name);
        }
        anvil_set_insert_point(ctx, func->entry);
        anvil_instr_t *dispatch = anvil_build_switch(ctx, anvil_func_get_param(func, 0), otherwise);
        for (size_t c = 0; dispatch && c < sizeof(cases) / sizeof(cases[0]); c++)
            anvil_switch_add_case(dispatch, anvil_const_i32(ctx, cases[c].value), blocks[cases[c].target]);
        for (int b = 0; b < 5; b++) {
            anvil_set_insert_point(ctx, blocks[b]);
            anvil_build_ret(ctx, anvil_const_i32(ctx, 100 + b));
        }
        anvil_set_insert_point(ctx, otherwise);
        anvil_build_ret(ctx, anvil_const_i32(ctx, -1));
        char *assembly = NULL;
        size_t length = 0;
        bool built = dispatch && anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
        size_t compares = built && targets[t].compare ? count_lines(assembly, targets[t].compare) : 0;
        char message[128];
        snprintf(message, sizeof(message), "arch %d: nine cases in five runs take at most six compares (%zu)", (int)targets[t].arch, compares);
        bool ok = built && (!targets[t].compare || (compares >= 1 && compares <= 6));
        CHECK(ok, message);
        if (!ok && assembly)
            fprintf(stderr, "%s\n", assembly);
        free(assembly);
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }
    if (failures) {
        fprintf(stderr, "switch_lowering_regression: %d failure(s)\n", failures);
        return 1;
    }
    printf("switch_lowering_regression: all checks passed\n");
    return 0;
}
