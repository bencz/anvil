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
    if (ctx->options.arch != MCC_ARCH_COUNT) return 6;
    if (mcc_ctx_has_feature(ctx, MCC_FEAT_LONG_DOUBLE) ||
        mcc_ctx_has_feature(ctx, MCC_FEAT_BITFIELDS) ||
        !mcc_ctx_has_feature(ctx, MCC_FEAT_PP_PRAGMA)) return 7;
    mcc_options_t options = ctx->options;
    options.arch = MCC_ARCH_X86_64;
    options.c_std = MCC_STD_C99;
    mcc_context_set_options(ctx, &options);
    if (mcc_ctx_has_feature(ctx, MCC_FEAT_COMPLEX)) return 8;
    options.c_std = MCC_STD_C11;
    mcc_context_set_options(ctx, &options);
    if (mcc_ctx_has_feature(ctx, MCC_FEAT_ALIGNAS) ||
        mcc_ctx_has_feature(ctx, MCC_FEAT_ATOMIC) ||
        mcc_ctx_has_feature(ctx, MCC_FEAT_THREAD_LOCAL)) return 9;
    options.c_std = MCC_STD_GNU99;
    mcc_context_set_options(ctx, &options);
    if (mcc_ctx_has_feature(ctx, MCC_FEAT_GNU_PACKED) ||
        mcc_ctx_has_feature(ctx, MCC_FEAT_GNU_COMPLEX)) return 10;
    if (mcc_alloc(ctx, SIZE_MAX) != NULL) return 6;
    if (!mcc_has_errors(ctx)) return 11;
    if (mcc_realloc(ctx, NULL, 0, SIZE_MAX) != NULL) return 12;
    if (mcc_realloc_array(ctx, NULL, SIZE_MAX, SIZE_MAX,
                          sizeof(uint64_t)) != NULL) return 13;
    if (mcc_alloc_array(ctx, SIZE_MAX, sizeof(uint64_t)) != NULL) return 14;
    if (!mcc_alloc(ctx, 32)) return 15;
    size_t saved_cap = ctx->cap_diagnostics;
    int saved_errors = ctx->error_count;
    ctx->num_diagnostics = SIZE_MAX;
    ctx->cap_diagnostics = SIZE_MAX;
    mcc_error(ctx, "diagnostic overflow injection");
    if (ctx->error_count != saved_errors + 1) return 16;
    ctx->num_diagnostics = 0;
    ctx->cap_diagnostics = saved_cap;
    mcc_context_destroy(ctx);
    return 0;
}
