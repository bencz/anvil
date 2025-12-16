/*
 * MCC - Micro C Compiler
 * Context management and utilities
 */

#include "mcc.h"

#define ARENA_INITIAL_SIZE (1024 * 1024)  /* 1MB */

/* Architecture names */
static const char *arch_names[] = {
    [MCC_ARCH_X86]      = "x86",
    [MCC_ARCH_X86_64]   = "x86_64",
    [MCC_ARCH_S370]     = "s370",
    [MCC_ARCH_S370_XA]  = "s370_xa",
    [MCC_ARCH_S390]     = "s390",
    [MCC_ARCH_ZARCH]    = "zarch",
    [MCC_ARCH_PPC32]    = "ppc32",
    [MCC_ARCH_PPC64]    = "ppc64",
    [MCC_ARCH_PPC64LE]  = "ppc64le",
    [MCC_ARCH_ARM64]    = "arm64",
};

mcc_context_t *mcc_context_create(void)
{
    mcc_context_t *ctx = calloc(1, sizeof(mcc_context_t));
    if (!ctx) return NULL;
    
    /* Initialize arena */
    ctx->arena = malloc(ARENA_INITIAL_SIZE);
    if (!ctx->arena) {
        free(ctx);
        return NULL;
    }
    ctx->arena_size = ARENA_INITIAL_SIZE;
    ctx->arena_used = 0;
    
    /* Initialize diagnostics */
    ctx->cap_diagnostics = 64;
    ctx->diagnostics = malloc(ctx->cap_diagnostics * sizeof(mcc_diagnostic_t));
    
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
    free(ctx->arena);
    
    free(ctx);
}

void mcc_context_set_options(mcc_context_t *ctx, const mcc_options_t *opts)
{
    if (!ctx || !opts) return;
    ctx->options = *opts;
}

/* Memory allocation */
void *mcc_alloc(mcc_context_t *ctx, size_t size)
{
    /* Align to 8 bytes */
    size = (size + 7) & ~7;
    
    /* Check if we need to grow arena */
    if (ctx->arena_used + size > ctx->arena_size) {
        size_t new_size = ctx->arena_size * 2;
        while (ctx->arena_used + size > new_size) {
            new_size *= 2;
        }
        void *new_arena = realloc(ctx->arena, new_size);
        if (!new_arena) {
            mcc_fatal(ctx, "Out of memory");
            return NULL;
        }
        ctx->arena = new_arena;
        ctx->arena_size = new_size;
    }
    
    void *ptr = (char*)ctx->arena + ctx->arena_used;
    ctx->arena_used += size;
    memset(ptr, 0, size);
    return ptr;
}

void *mcc_realloc(mcc_context_t *ctx, void *ptr, size_t old_size, size_t new_size)
{
    if (!ptr) return mcc_alloc(ctx, new_size);
    
    void *new_ptr = mcc_alloc(ctx, new_size);
    if (new_ptr && ptr) {
        memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    }
    return new_ptr;
}

char *mcc_strdup(mcc_context_t *ctx, const char *str)
{
    if (!str) return NULL;
    size_t len = strlen(str);
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
    if (ctx->num_diagnostics >= ctx->cap_diagnostics) {
        ctx->cap_diagnostics *= 2;
        ctx->diagnostics = realloc(ctx->diagnostics,
                                    ctx->cap_diagnostics * sizeof(mcc_diagnostic_t));
    }
    
    mcc_diagnostic_t *diag = &ctx->diagnostics[ctx->num_diagnostics++];
    diag->severity = sev;
    diag->location = loc;
    
    /* Format message */
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    diag->message = strdup(buf);
    
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
    if (arch < MCC_ARCH_COUNT) {
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
