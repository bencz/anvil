/* One architecture descriptor: every backend's get_arch_info returns the
 * core table's entry (what anvil_ctx_get_arch_info reads), so the facts the
 * passes shape code for cannot drift from a stale backend copy. */
#include "anvil/anvil_internal.h"
#include <stdio.h>

int main(void)
{
    int failures = 0;
    for (int arch = 0; arch < ANVIL_ARCH_COUNT; arch++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target((anvil_arch_t)arch);
        const anvil_arch_info_t *core = anvil_ctx_get_arch_info(ctx);
        const anvil_arch_info_t *backend = ctx && ctx->backend && ctx->backend->ops->get_arch_info ? ctx->backend->ops->get_arch_info(ctx->backend) : NULL;
        if (!core || backend != core) {
            fprintf(stderr, "[FAIL] arch %d: the backend's descriptor is not the core one\n", arch);
            failures++;
        }
        anvil_ctx_destroy(ctx);
    }
    if (failures)
        return 1;
    printf("arch info regression tests passed\n");
    return 0;
}
