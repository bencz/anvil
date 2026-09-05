/*
 * MCC - Micro C Compiler
 * Context management and utilities
 */

#include "anvil/anvil.h"
#include "mcc.h"

#define ARENA_INITIAL_SIZE (1024 * 1024)  /* 1MB */
#define ARENA_ALIGN 16

struct mcc_arena_block {
    struct mcc_arena_block *next;
    size_t size;
    size_t used;
    char data[];
};

static struct mcc_arena_block *arena_block_new(size_t payload)
{
    if (payload > SIZE_MAX - sizeof(struct mcc_arena_block)) return NULL;
    size_t total = sizeof(struct mcc_arena_block) + payload;
    struct mcc_arena_block *blk = malloc(total);
    if (!blk) return NULL;
    blk->next = NULL;
    blk->size = payload;
    blk->used = 0;
    return blk;
}

/* Architecture names */
static const char *arch_names[] = {
    [MCC_ARCH_X86] = "x86",
    [MCC_ARCH_X86_64] = "x86_64",
    [MCC_ARCH_S370] = "s370",
    [MCC_ARCH_S370_XA] = "s370_xa",
    [MCC_ARCH_S390] = "s390",
    [MCC_ARCH_ZARCH] = "zarch",
    [MCC_ARCH_PPC32] = "ppc32",
    [MCC_ARCH_PPC64] = "ppc64",
    [MCC_ARCH_PPC64LE] = "ppc64le",
    [MCC_ARCH_ARM64] = "arm64",
    [MCC_ARCH_ARM64_MACOS] = "arm64-macos",
    [MCC_ARCH_X86_64_WINDOWS] = "x86_64_windows",
};

/*
 * Update effective C standard features based on options
 * Similar to ANVIL's update_cpu_features()
 */
static void update_c_features(mcc_context_t *ctx)
{
    /* Resolve the standard (DEFAULT -> C89) */
    ctx->effective_std = mcc_c_std_resolve(ctx->options.c_std);
    
    /* Get base features for the standard */
    mcc_c_std_get_features(ctx->effective_std, &ctx->effective_features);
    
    /* Apply overrides: enable first, then disable */
    mcc_features_or(&ctx->effective_features, &ctx->features_enabled);
    mcc_features_remove(&ctx->effective_features, &ctx->features_disabled);
}

mcc_context_t *mcc_context_create(void)
{
    mcc_context_t *ctx = calloc(1, sizeof(mcc_context_t));
    if (!ctx) return NULL;
    
    /* Initialize arena */
    ctx->arena = arena_block_new(ARENA_INITIAL_SIZE);
    if (!ctx->arena) {
        free(ctx);
        return NULL;
    }
    ctx->arena_size = ARENA_INITIAL_SIZE;
    ctx->arena_used = 0;
    
    /* Initialize diagnostics */
    ctx->cap_diagnostics = 64;
    ctx->diagnostics = malloc(ctx->cap_diagnostics * sizeof(mcc_diagnostic_t));
    if (!ctx->diagnostics) {
        free(ctx->arena);
        free(ctx);
        return NULL;
    }
    
    /* Initialize C standard to default (C89) */
    ctx->options.arch = MCC_ARCH_COUNT;
    ctx->options.c_std = MCC_STD_DEFAULT;
    update_c_features(ctx);
    
    return ctx;
}

void mcc_context_destroy(mcc_context_t *ctx)
{
    if (!ctx) return;
    
    /* Free diagnostics */
    for (size_t i = 0; i < ctx->num_diagnostics; i++) {
        free(ctx->diagnostics[i].message);
    }
    free(ctx->diagnostics);
    
    /* Free arena */
    struct mcc_arena_block *blk = ctx->arena;
    while (blk) {
        struct mcc_arena_block *next = blk->next;
        free(blk);
        blk = next;
    }

    free(ctx);
}

void mcc_context_set_options(mcc_context_t *ctx, const mcc_options_t *opts)
{
    if (!ctx || !opts) return;
    ctx->options = *opts;
    
    /* Update effective C standard features */
    update_c_features(ctx);
}

/* C Standard feature checking API */
bool mcc_ctx_has_feature(mcc_context_t *ctx, mcc_feature_id_t feature)
{
    if (!ctx) return false;
    return MCC_FEATURES_HAS(ctx->effective_features, feature);
}

mcc_c_std_t mcc_ctx_get_std(mcc_context_t *ctx)
{
    if (!ctx) return MCC_STD_DEFAULT;
    return ctx->effective_std;
}

const char *mcc_ctx_get_std_name(mcc_context_t *ctx)
{
    if (!ctx) return "unknown";
    return mcc_c_std_get_name(ctx->effective_std);
}

/* Feature override functions */
void mcc_ctx_enable_feature(mcc_context_t *ctx, mcc_feature_id_t feature)
{
    if (!ctx) return;
    MCC_FEATURES_SET(ctx->features_enabled, feature);
    MCC_FEATURES_CLEAR(ctx->features_disabled, feature);
    update_c_features(ctx);
}

void mcc_ctx_disable_feature(mcc_context_t *ctx, mcc_feature_id_t feature)
{
    if (!ctx) return;
    MCC_FEATURES_SET(ctx->features_disabled, feature);
    MCC_FEATURES_CLEAR(ctx->features_enabled, feature);
    update_c_features(ctx);
}

/* Memory allocation */
void *mcc_alloc(mcc_context_t *ctx, size_t size)
{
    if (!ctx || !ctx->arena) return NULL;

    /* Align to ARENA_ALIGN bytes */
    if (size == 0) size = 1;
    if (size > SIZE_MAX - (ARENA_ALIGN - 1)) {
        mcc_fatal(ctx, "Arena allocation size overflow");
        return NULL;
    }
    size = (size + (ARENA_ALIGN - 1)) & ~((size_t)ARENA_ALIGN - 1);

    struct mcc_arena_block *cur = ctx->arena;
    if (cur->used > cur->size) {
        mcc_fatal(ctx, "Corrupt arena block accounting");
        return NULL;
    }

    /* Check if the current block can satisfy the request */
    if (size > cur->size - cur->used) {
        /* Oversized requests get a dedicated block; otherwise grow by a
           fresh default-sized block. Existing blocks never move. */
        size_t payload = size > ARENA_INITIAL_SIZE ? size : ARENA_INITIAL_SIZE;
        struct mcc_arena_block *blk = arena_block_new(payload);
        if (!blk) {
            mcc_fatal(ctx, "Out of memory");
            return NULL;
        }
        blk->next = cur;
        ctx->arena = blk;
        cur = blk;
    }

    void *ptr = cur->data + cur->used;
    cur->used += size;
    ctx->arena_size = cur->size;
    ctx->arena_used = cur->used;
    memset(ptr, 0, size);
    return ptr;
}

void *mcc_realloc(mcc_context_t *ctx, void *ptr, size_t old_size, size_t new_size)
{
    if (!ctx || !ctx->arena) return NULL;
    if (!ptr) return mcc_alloc(ctx, new_size);
    
    void *new_ptr = mcc_alloc(ctx, new_size);
    if (new_ptr && ptr) {
        memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    }
    return new_ptr;
}

void *mcc_alloc_array(mcc_context_t *ctx, size_t count,
                      size_t element_size)
{
    if (!ctx || element_size == 0 || count > SIZE_MAX / element_size) {
        if (ctx) mcc_fatal(ctx, "Array allocation size overflow");
        return NULL;
    }
    return mcc_alloc(ctx, count * element_size);
}

void *mcc_realloc_array(mcc_context_t *ctx, void *ptr,
                        size_t old_count, size_t new_count,
                        size_t element_size)
{
    if (!ctx || element_size == 0 ||
        old_count > SIZE_MAX / element_size ||
        new_count > SIZE_MAX / element_size) {
        if (ctx) mcc_fatal(ctx, "Array allocation size overflow");
        return NULL;
    }
    return mcc_realloc(ctx, ptr, old_count * element_size,
                       new_count * element_size);
}

char *mcc_strdup(mcc_context_t *ctx, const char *str)
{
    if (!ctx || !str) return NULL;
    size_t len = strlen(str);
    if (len == SIZE_MAX) {
        mcc_fatal(ctx, "String allocation size overflow");
        return NULL;
    }
    char *copy = mcc_alloc(ctx, len + 1);
    if (copy) {
        memcpy(copy, str, len + 1);
    }
    return copy;
}

/* Diagnostics */
static void mcc_add_diagnostic(mcc_context_t *ctx, mcc_severity_t sev,
                                mcc_location_t loc, const char *fmt, va_list args)
{
    if (!ctx || !fmt) return;
    if (ctx->num_diagnostics >= ctx->cap_diagnostics) {
        if (ctx->cap_diagnostics > SIZE_MAX / 2) {
            fprintf(stderr, "fatal error: diagnostic capacity overflow\n");
            ctx->error_count++;
            return;
        }
        size_t new_capacity = ctx->cap_diagnostics * 2;
        if (new_capacity > SIZE_MAX / sizeof(mcc_diagnostic_t)) {
            fprintf(stderr, "fatal error: diagnostic allocation overflow\n");
            ctx->error_count++;
            return;
        }
        void *new_diagnostics = realloc(
            ctx->diagnostics, new_capacity * sizeof(mcc_diagnostic_t));
        if (!new_diagnostics) {
            fprintf(stderr, "fatal error: out of memory growing diagnostics\n");
            ctx->error_count++;
            return;
        }
        ctx->diagnostics = new_diagnostics;
        ctx->cap_diagnostics = new_capacity;
    }
    
    mcc_diagnostic_t *diag = &ctx->diagnostics[ctx->num_diagnostics++];
    diag->severity = sev;
    diag->location = loc;
    
    /* Format message */
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    diag->message = strdup(buf);
    if (!diag->message) {
        ctx->num_diagnostics--;
        fprintf(stderr, "fatal error: out of memory recording diagnostic: %s\n",
                buf);
        ctx->error_count++;
        return;
    }
    
    /* Print to stderr */
    const char *sev_str = "";
    switch (sev) {
        case MCC_SEV_NOTE:    sev_str = "note"; break;
        case MCC_SEV_WARNING: sev_str = "warning"; break;
        case MCC_SEV_ERROR:   sev_str = "error"; break;
        case MCC_SEV_FATAL:   sev_str = "fatal error"; break;
    }
    
    if (loc.filename) {
        fprintf(stderr, "%s:%d:%d: %s: %s\n",
                loc.filename, loc.line, loc.column, sev_str, buf);
    } else {
        fprintf(stderr, "%s: %s\n", sev_str, buf);
    }
    
    /* Update counts */
    if (sev == MCC_SEV_ERROR || sev == MCC_SEV_FATAL) {
        ctx->error_count++;
    } else if (sev == MCC_SEV_WARNING) {
        ctx->warning_count++;
        if (ctx->options.warn_error) {
            ctx->error_count++;
        }
    }
}

void mcc_error(mcc_context_t *ctx, const char *fmt, ...)
{
    mcc_location_t loc = {ctx->current_file, ctx->current_line, ctx->current_column};
    va_list args;
    va_start(args, fmt);
    mcc_add_diagnostic(ctx, MCC_SEV_ERROR, loc, fmt, args);
    va_end(args);
}

void mcc_error_at(mcc_context_t *ctx, mcc_location_t loc, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    mcc_add_diagnostic(ctx, MCC_SEV_ERROR, loc, fmt, args);
    va_end(args);
}

void mcc_warning(mcc_context_t *ctx, const char *fmt, ...)
{
    mcc_location_t loc = {ctx->current_file, ctx->current_line, ctx->current_column};
    va_list args;
    va_start(args, fmt);
    mcc_add_diagnostic(ctx, MCC_SEV_WARNING, loc, fmt, args);
    va_end(args);
}

void mcc_warning_at(mcc_context_t *ctx, mcc_location_t loc, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    mcc_add_diagnostic(ctx, MCC_SEV_WARNING, loc, fmt, args);
    va_end(args);
}

void mcc_note(mcc_context_t *ctx, const char *fmt, ...)
{
    mcc_location_t loc = {ctx->current_file, ctx->current_line, ctx->current_column};
    va_list args;
    va_start(args, fmt);
    mcc_add_diagnostic(ctx, MCC_SEV_NOTE, loc, fmt, args);
    va_end(args);
}

void mcc_fatal(mcc_context_t *ctx, const char *fmt, ...)
{
    mcc_location_t loc = {ctx->current_file, ctx->current_line, ctx->current_column};
    va_list args;
    va_start(args, fmt);
    mcc_add_diagnostic(ctx, MCC_SEV_FATAL, loc, fmt, args);
    va_end(args);
}

bool mcc_has_errors(mcc_context_t *ctx)
{
    return ctx->error_count > 0;
}

/* Utilities */
const char *mcc_arch_name(mcc_arch_t arch)
{
    if ((unsigned)arch < MCC_ARCH_COUNT)
    {
        return arch_names[arch];
    }
    return "unknown";
}

mcc_arch_t mcc_arch_from_name(const char *name)
{
    for (int i = 0; i < MCC_ARCH_COUNT; i++) {
        if (strcmp(arch_names[i], name) == 0) {
            return (mcc_arch_t)i;
        }
    }
    return MCC_ARCH_COUNT;
}

/* Map MCC architecture to ANVIL architecture */
anvil_arch_t mcc_arch_to_anvil(mcc_arch_t arch)
{
    switch (arch) {
        case MCC_ARCH_X86:         return ANVIL_ARCH_X86;
        case MCC_ARCH_X86_64:      return ANVIL_ARCH_X86_64;
        case MCC_ARCH_X86_64_WINDOWS:
            return ANVIL_ARCH_X86_64;
        case MCC_ARCH_S370:        return ANVIL_ARCH_S370;
        case MCC_ARCH_S370_XA:     return ANVIL_ARCH_S370_XA;
        case MCC_ARCH_S390:        return ANVIL_ARCH_S390;
        case MCC_ARCH_ZARCH:       return ANVIL_ARCH_ZARCH;
        case MCC_ARCH_PPC32:       return ANVIL_ARCH_PPC32;
        case MCC_ARCH_PPC64:       return ANVIL_ARCH_PPC64;
        case MCC_ARCH_PPC64LE:     return ANVIL_ARCH_PPC64LE;
        case MCC_ARCH_ARM64:       return ANVIL_ARCH_ARM64;
        case MCC_ARCH_ARM64_MACOS: return ANVIL_ARCH_ARM64;
        default:                   return ANVIL_ARCH_NONE;
    }
}
