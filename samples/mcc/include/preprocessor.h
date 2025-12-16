/*
 * MCC - Micro C Compiler
 * Preprocessor interface
 */

#ifndef MCC_PREPROCESSOR_H
#define MCC_PREPROCESSOR_H

/* Forward declarations */
typedef struct mcc_preprocessor mcc_preprocessor_t;
typedef struct mcc_macro mcc_macro_t;

/* Macro parameter */
typedef struct mcc_macro_param {
    const char *name;
    struct mcc_macro_param *next;
} mcc_macro_param_t;

/* Macro definition */
struct mcc_macro {
    const char *name;
    bool is_function_like;      /* true if macro has parameters */
    bool is_variadic;           /* true if last param is ... */
    mcc_macro_param_t *params;  /* Parameter list */
    int num_params;
    mcc_token_t *body;          /* Replacement token list */
    mcc_location_t def_loc;     /* Where macro was defined */
    struct mcc_macro *next;     /* Hash chain */
};

/* Include file info */
typedef struct mcc_include_file {
    const char *filename;
    const char *content;
    const char *pos;            /* Current position in content */
    int line;                   /* Current line number */
    int column;                 /* Current column number */
    struct mcc_include_file *next;
} mcc_include_file_t;

/* Conditional stack entry */
typedef struct mcc_cond_stack {
    bool condition;             /* Current condition value */
    bool has_else;              /* Has #else been seen? */
    bool any_true;              /* Has any branch been true? */
    mcc_location_t location;    /* Location of #if/#ifdef */
    struct mcc_cond_stack *next;
} mcc_cond_stack_t;

/* Preprocessor state */
struct mcc_preprocessor {
    mcc_context_t *ctx;
    mcc_lexer_t *lexer;
    
    /* Macro table (hash table) */
    mcc_macro_t **macros;
    size_t macro_table_size;
    
    /* Include stack */
    mcc_include_file_t *include_stack;
    int include_depth;
    
    /* Conditional compilation stack */
    mcc_cond_stack_t *cond_stack;
    bool skip_mode;             /* Currently skipping tokens? */
    
    /* Include paths */
    const char **include_paths;
    size_t num_include_paths;
    
    /* Token buffer for output */
    mcc_token_t *output_head;
    mcc_token_t *output_tail;
    
    /* Current token being processed */
    mcc_token_t *current;
    
    /* Macro expansion state */
    bool in_macro_expansion;
    const char **expanding_macros;  /* Stack of macros being expanded */
    size_t num_expanding;
    
    /* Space tracking for macro expansion */
    bool next_has_space;            /* has_space for next emitted token */
    bool use_next_has_space;        /* Whether to use next_has_space */
};

/* Preprocessor lifecycle */
mcc_preprocessor_t *mcc_preprocessor_create(mcc_context_t *ctx);
void mcc_preprocessor_destroy(mcc_preprocessor_t *pp);

/* Configuration */
void mcc_preprocessor_add_include_path(mcc_preprocessor_t *pp, const char *path);
void mcc_preprocessor_define(mcc_preprocessor_t *pp, const char *name, const char *value);
void mcc_preprocessor_undef(mcc_preprocessor_t *pp, const char *name);

/* Processing */
mcc_token_t *mcc_preprocessor_run(mcc_preprocessor_t *pp, const char *filename);
mcc_token_t *mcc_preprocessor_run_string(mcc_preprocessor_t *pp, const char *source, const char *filename);

/* Token retrieval (after preprocessing) */
mcc_token_t *mcc_preprocessor_next(mcc_preprocessor_t *pp);
mcc_token_t *mcc_preprocessor_peek(mcc_preprocessor_t *pp);

/* Macro operations */
mcc_macro_t *mcc_preprocessor_lookup_macro(mcc_preprocessor_t *pp, const char *name);
bool mcc_preprocessor_is_defined(mcc_preprocessor_t *pp, const char *name);

/* Predefined macros */
void mcc_preprocessor_define_builtins(mcc_preprocessor_t *pp);

#endif /* MCC_PREPROCESSOR_H */
