#ifndef ANVIL_ERROR_H
#define ANVIL_ERROR_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ANVIL_SEV_ERROR,
    ANVIL_SEV_WARNING,
    ANVIL_SEV_NOTE
} AnvilSeverity;

typedef struct AnvilDiagnostic {
    AnvilSeverity severity;
    const char* message;
    const char* function_name;
    int line;
    struct AnvilDiagnostic* next;
} AnvilDiagnostic;

typedef struct AnvilErrorCtx {
    AnvilDiagnostic* first;
    AnvilDiagnostic* last;
    int error_count;
    int warning_count;
} AnvilErrorCtx;

void anvil_error_ctx_init(AnvilErrorCtx* ctx);
void anvil_error_ctx_free(AnvilErrorCtx* ctx);
void anvil_error_ctx_clear(AnvilErrorCtx* ctx);

void anvil_error_add(AnvilErrorCtx* ctx, AnvilSeverity sev, 
                     const char* func, int line, const char* msg);
void anvil_error_addf(AnvilErrorCtx* ctx, AnvilSeverity sev,
                      const char* func, int line, const char* fmt, ...);

bool anvil_error_has_errors(const AnvilErrorCtx* ctx);
int anvil_error_count(const AnvilErrorCtx* ctx);
char* anvil_error_format_all(const AnvilErrorCtx* ctx);

void anvil_fatal(const char* msg);
void anvil_fatalf(const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
