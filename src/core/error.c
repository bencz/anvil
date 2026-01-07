#include "error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

void anvil_error_ctx_init(AnvilErrorCtx* ctx) {
    ctx->first = NULL;
    ctx->last = NULL;
    ctx->error_count = 0;
    ctx->warning_count = 0;
}

void anvil_error_ctx_free(AnvilErrorCtx* ctx) {
    AnvilDiagnostic* diag = ctx->first;
    while (diag) {
        AnvilDiagnostic* next = diag->next;
        free((void*)diag->message);
        free(diag);
        diag = next;
    }
    ctx->first = NULL;
    ctx->last = NULL;
    ctx->error_count = 0;
    ctx->warning_count = 0;
}

void anvil_error_ctx_clear(AnvilErrorCtx* ctx) {
    anvil_error_ctx_free(ctx);
    anvil_error_ctx_init(ctx);
}

void anvil_error_add(AnvilErrorCtx* ctx, AnvilSeverity sev,
                     const char* func, int line, const char* msg) {
    AnvilDiagnostic* diag = (AnvilDiagnostic*)malloc(sizeof(AnvilDiagnostic));
    diag->severity = sev;
    diag->message = msg ? strdup(msg) : NULL;
    diag->function_name = func;
    diag->line = line;
    diag->next = NULL;
    
    if (ctx->last) {
        ctx->last->next = diag;
        ctx->last = diag;
    } else {
        ctx->first = ctx->last = diag;
    }
    
    if (sev == ANVIL_SEV_ERROR) ctx->error_count++;
    else if (sev == ANVIL_SEV_WARNING) ctx->warning_count++;
}

void anvil_error_addf(AnvilErrorCtx* ctx, AnvilSeverity sev,
                      const char* func, int line, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    
    char* msg = (char*)malloc(len + 1);
    va_start(args, fmt);
    vsnprintf(msg, len + 1, fmt, args);
    va_end(args);
    
    AnvilDiagnostic* diag = (AnvilDiagnostic*)malloc(sizeof(AnvilDiagnostic));
    diag->severity = sev;
    diag->message = msg;
    diag->function_name = func;
    diag->line = line;
    diag->next = NULL;
    
    if (ctx->last) {
        ctx->last->next = diag;
        ctx->last = diag;
    } else {
        ctx->first = ctx->last = diag;
    }
    
    if (sev == ANVIL_SEV_ERROR) ctx->error_count++;
    else if (sev == ANVIL_SEV_WARNING) ctx->warning_count++;
}

bool anvil_error_has_errors(const AnvilErrorCtx* ctx) {
    return ctx->error_count > 0;
}

int anvil_error_count(const AnvilErrorCtx* ctx) {
    return ctx->error_count;
}

char* anvil_error_format_all(const AnvilErrorCtx* ctx) {
    size_t total_len = 0;
    for (AnvilDiagnostic* d = ctx->first; d; d = d->next) {
        total_len += 256;
        if (d->message) total_len += strlen(d->message);
    }
    if (total_len == 0) return NULL;
    
    char* result = (char*)malloc(total_len);
    result[0] = '\0';
    char* ptr = result;
    
    for (AnvilDiagnostic* d = ctx->first; d; d = d->next) {
        const char* sev_str = d->severity == ANVIL_SEV_ERROR ? "error" :
                              d->severity == ANVIL_SEV_WARNING ? "warning" : "note";
        int written;
        if (d->function_name && d->line > 0) {
            written = sprintf(ptr, "%s:%d: %s: %s\n", 
                             d->function_name, d->line, sev_str, 
                             d->message ? d->message : "");
        } else if (d->function_name) {
            written = sprintf(ptr, "%s: %s: %s\n",
                             d->function_name, sev_str,
                             d->message ? d->message : "");
        } else {
            written = sprintf(ptr, "%s: %s\n", sev_str,
                             d->message ? d->message : "");
        }
        ptr += written;
    }
    return result;
}

void anvil_fatal(const char* msg) {
    fprintf(stderr, "ANVIL FATAL: %s\n", msg);
    abort();
}

void anvil_fatalf(const char* fmt, ...) {
    fprintf(stderr, "ANVIL FATAL: ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    abort();
}
