/*
 * MCC - Micro C Compiler
 * Main header file
 */

#ifndef MCC_H
#define MCC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <ctype.h>
#include <assert.h>

#include <anvil/anvil.h>

/* Version */
#define MCC_VERSION_MAJOR 0
#define MCC_VERSION_MINOR 2
#define MCC_VERSION_PATCH 0
#define MCC_VERSION_STRING "0.2.0"

/* C Standard support header (must be included before other MCC headers) */
#include "c_std.h"

/* Limits */
#define MCC_MAX_IDENT_LEN    256
#define MCC_MAX_STRING_LEN   4096
#define MCC_MAX_INCLUDE_DEPTH 64
#define MCC_MAX_MACRO_ARGS   127
#define MCC_MAX_ERRORS       100

/* Forward declarations */
typedef struct mcc_context mcc_context_t;
typedef struct mcc_options mcc_options_t;

/* Target architecture (matches ANVIL) */
typedef enum {
    MCC_ARCH_X86,
    MCC_ARCH_X86_64,
    MCC_ARCH_S370,
    MCC_ARCH_S370_XA,
    MCC_ARCH_S390,
    MCC_ARCH_ZARCH,
    MCC_ARCH_PPC32,
    MCC_ARCH_PPC64,
    MCC_ARCH_PPC64LE,
    MCC_ARCH_ARM64,
    MCC_ARCH_ARM64_MACOS,   /* ARM64 with Darwin ABI (Apple Silicon) */
    MCC_ARCH_COUNT
} mcc_arch_t;

/* Optimization level */
typedef enum {
    MCC_OPT_NONE = 0,
    MCC_OPT_BASIC = 1,
    MCC_OPT_STANDARD = 2,
    MCC_OPT_AGGRESSIVE = 3
} mcc_opt_level_t;

/* Compiler options */
struct mcc_options {
    mcc_arch_t arch;
    mcc_opt_level_t opt_level;
    mcc_c_std_t c_std;              /* C language standard (-std=) */
    
    /* Output control */
    const char *output_file;
    bool preprocess_only;       /* -E */
    bool syntax_only;           /* -fsyntax-only */
    bool emit_ast;              /* -ast-dump */
    bool dump_ir;               /* -dump-ir */
    
    /* Input files (multiple file support) */
    const char **input_files;
    size_t num_input_files;
    
    /* Include paths */
    const char **include_paths;
    size_t num_include_paths;
    
    /* Defines (-D) */
    const char **defines;
    size_t num_defines;
    
    /* Warnings */
    bool warn_all;              /* -Wall */
    bool warn_extra;            /* -Wextra */
    bool warn_error;            /* -Werror */
    
    /* Debug */
    bool verbose;
    bool debug_lexer;
    bool debug_parser;
    bool debug_codegen;
};

/* Error/warning severity */
typedef enum {
    MCC_SEV_NOTE,
    MCC_SEV_WARNING,
    MCC_SEV_ERROR,
    MCC_SEV_FATAL
} mcc_severity_t;

/* Source location */
typedef struct {
    const char *filename;
    int line;
    int column;
} mcc_location_t;

/* Diagnostic message */
typedef struct {
    mcc_severity_t severity;
    mcc_location_t location;
    char *message;
} mcc_diagnostic_t;

/* Compiler context */
struct mcc_context {
    mcc_options_t options;
    
    /* Effective C standard and features (computed from options) */
    mcc_c_std_t effective_std;
    mcc_c_features_t effective_features;
    
    /* Feature overrides */
    mcc_c_features_t features_enabled;   /* Features to enable beyond standard */
    mcc_c_features_t features_disabled;  /* Features to disable from standard */
    
    /* Diagnostics */
    mcc_diagnostic_t *diagnostics;
    size_t num_diagnostics;
    size_t cap_diagnostics;
    int error_count;
    int warning_count;
    
    /* Current file info */
    const char *current_file;
    int current_line;
    int current_column;
    
    /* Memory arena for allocations */
    void *arena;
    size_t arena_size;
    size_t arena_used;
};

/* Context management */
mcc_context_t *mcc_context_create(void);
void mcc_context_destroy(mcc_context_t *ctx);
void mcc_context_set_options(mcc_context_t *ctx, const mcc_options_t *opts);

/* C Standard feature checking */
bool mcc_ctx_has_feature(mcc_context_t *ctx, mcc_feature_id_t feature);
mcc_c_std_t mcc_ctx_get_std(mcc_context_t *ctx);
const char *mcc_ctx_get_std_name(mcc_context_t *ctx);

/* Feature override functions */
void mcc_ctx_enable_feature(mcc_context_t *ctx, mcc_feature_id_t feature);
void mcc_ctx_disable_feature(mcc_context_t *ctx, mcc_feature_id_t feature);

/* Diagnostics */
void mcc_error(mcc_context_t *ctx, const char *fmt, ...);
void mcc_error_at(mcc_context_t *ctx, mcc_location_t loc, const char *fmt, ...);
void mcc_warning(mcc_context_t *ctx, const char *fmt, ...);
void mcc_warning_at(mcc_context_t *ctx, mcc_location_t loc, const char *fmt, ...);
void mcc_note(mcc_context_t *ctx, const char *fmt, ...);
void mcc_fatal(mcc_context_t *ctx, const char *fmt, ...);
bool mcc_has_errors(mcc_context_t *ctx);

/* Memory allocation (arena-based) */
void *mcc_alloc(mcc_context_t *ctx, size_t size);
void *mcc_realloc(mcc_context_t *ctx, void *ptr, size_t old_size, size_t new_size);
char *mcc_strdup(mcc_context_t *ctx, const char *str);

/* Utility */
const char *mcc_arch_name(mcc_arch_t arch);
mcc_arch_t mcc_arch_from_name(const char *name);
anvil_arch_t mcc_arch_to_anvil(mcc_arch_t arch);

/* Include component headers */
#include "token.h"
#include "lexer.h"
#include "preprocessor.h"
#include "ast.h"
#include "parser.h"
#include "types.h"
#include "symtab.h"
#include "sema.h"
#include "codegen.h"

#endif /* MCC_H */
