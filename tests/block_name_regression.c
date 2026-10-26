/* Block names are unique within a function: passes that create one
 * "loop.preheader" per loop must not emit colliding labels. */
#include <anvil/anvil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "[FAIL] %s\n", (m)); failures++; } } while (0)

int main(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *mod = anvil_module_create(ctx, "block_names");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *fn_type = anvil_type_func(ctx, i32, &i32, 1, false);
    anvil_func_t *fn = anvil_func_create(mod, "pick", fn_type,
                                         ANVIL_LINK_EXTERNAL);
    anvil_func_t *other = anvil_func_create(mod, "other", fn_type,
                                            ANVIL_LINK_EXTERNAL);

    anvil_block_t *a = anvil_block_create(fn, "dup");
    anvil_block_t *b = anvil_block_create(fn, "dup");
    anvil_block_t *c = anvil_block_create(fn, "dup.1");
    anvil_block_t *d = anvil_block_create(other, "dup");
    CHECK(a && strcmp(anvil_block_get_name(a), "dup") == 0, "first keeps name");
    CHECK(b && strcmp(anvil_block_get_name(b), "dup.1") == 0, "repeat gets suffix");
    CHECK(c && strcmp(anvil_block_get_name(c), "dup.1.1") == 0,
          "suffix itself is not reused");
    CHECK(d && strcmp(anvil_block_get_name(d), "dup") == 0,
          "names are per function");

    /* Many repeats exercise growth of the name table. */
    char expected[32];
    for (int i = 0; i < 200; i++) {
        anvil_block_t *block = anvil_block_create(fn, "loop.preheader");
        if (i == 0) snprintf(expected, sizeof(expected), "loop.preheader");
        else snprintf(expected, sizeof(expected), "loop.preheader.%d", i);
        if (!block || strcmp(anvil_block_get_name(block), expected) != 0) {
            CHECK(false, "repeated names are numbered in order");
            break;
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    if (failures) {
        fprintf(stderr, "block name regression: %d failure(s)\n", failures);
        return 1;
    }
    puts("block name regression: PASS");
    return 0;
}
