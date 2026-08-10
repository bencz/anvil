#include "mcc.h"
#include <stdint.h>

int main(void)
{
    if (mcc_arch_to_anvil(MCC_ARCH_COUNT) != ANVIL_ARCH_NONE) return 1;
    if (mcc_arch_to_anvil((mcc_arch_t)-1) != ANVIL_ARCH_NONE) return 2;
    if (mcc_arch_to_anvil(MCC_ARCH_X86_64) != ANVIL_ARCH_X86_64) return 3;

    if (mcc_alloc(NULL, 1) != NULL) return 4;
    mcc_context_t *ctx = mcc_context_create();
    if (!ctx) return 5;
    if (mcc_alloc(ctx, SIZE_MAX) != NULL) return 6;
    if (!mcc_has_errors(ctx)) return 7;
    if (mcc_realloc(ctx, NULL, 0, SIZE_MAX) != NULL) return 8;
    if (!mcc_alloc(ctx, 32)) return 9;
    size_t saved_cap = ctx->cap_diagnostics;
    int saved_errors = ctx->error_count;
    ctx->num_diagnostics = SIZE_MAX;
    ctx->cap_diagnostics = SIZE_MAX;
    mcc_error(ctx, "diagnostic overflow injection");
    if (ctx->error_count != saved_errors + 1) return 10;
    ctx->num_diagnostics = 0;
    ctx->cap_diagnostics = saved_cap;
    mcc_context_destroy(ctx);
    return 0;
}
