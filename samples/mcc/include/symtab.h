/*
 * MCC - Micro C Compiler
 * Symbol Table
 */

#ifndef MCC_SYMTAB_H
#define MCC_SYMTAB_H

/* Symbol kinds */
typedef enum {
    SYM_VAR,            /* Variable */
    SYM_FUNC,           /* Function */
    SYM_PARAM,          /* Function parameter */
    SYM_TYPEDEF,        /* Typedef name */
    SYM_STRUCT,         /* Struct tag */
    SYM_UNION,          /* Union tag */
    SYM_ENUM,           /* Enum tag */
    SYM_ENUM_CONST,     /* Enum constant */
    SYM_LABEL,          /* Label (goto target) */
    
    SYM_COUNT
} mcc_sym_kind_t;

/* Forward declarations */
typedef struct mcc_symbol mcc_symbol_t;
typedef struct mcc_scope mcc_scope_t;
typedef struct mcc_symtab mcc_symtab_t;

/* Symbol structure */
struct mcc_symbol {
    mcc_sym_kind_t kind;
    const char *name;
    mcc_type_t *type;
    mcc_location_t location;    /* Where symbol was declared */
    
    /* Storage info */
    mcc_storage_class_t storage;
    
    /* For variables: stack offset or global name */
    union {
        int stack_offset;       /* Local variable offset */
        const char *global_name; /* Global variable/function name */
        int enum_value;         /* Enum constant value */
    } data;
    
    /* Flags */
    bool is_defined;            /* Has definition (vs just declaration) */
    bool is_used;               /* Has been referenced */
    bool is_parameter;          /* Is function parameter */
    
    /* AST node (for functions with bodies) */
    mcc_ast_node_t *ast_node;
    
    /* Hash chain */
    mcc_symbol_t *next;
};

/* Scope structure */
struct mcc_scope {
    mcc_scope_t *parent;        /* Enclosing scope */
    
    /* Symbol hash table */
    mcc_symbol_t **symbols;
    size_t table_size;
    size_t num_symbols;
    
    /* Tag namespace (struct/union/enum) */
    mcc_symbol_t **tags;
    size_t tag_table_size;
    size_t num_tags;
    
    /* Label namespace (function scope only) */
    mcc_symbol_t **labels;
    size_t label_table_size;
    size_t num_labels;
    
    /* Scope info */
    int depth;                  /* Nesting depth */
    bool is_file_scope;         /* Global scope */
    bool is_function_scope;     /* Function body scope */
    bool is_block_scope;        /* Block scope */
    
    /* For local variables: current stack offset */
    int stack_offset;
};

/* Symbol table structure */
struct mcc_symtab {
    mcc_context_t *ctx;
    mcc_scope_t *current;       /* Current scope */
    mcc_scope_t *global;        /* Global/file scope */
    
    /* Type context */
    mcc_type_context_t *types;
};

/* Symbol table lifecycle */
mcc_symtab_t *mcc_symtab_create(mcc_context_t *ctx, mcc_type_context_t *types);
void mcc_symtab_destroy(mcc_symtab_t *symtab);

/* Scope management */
void mcc_symtab_push_scope(mcc_symtab_t *symtab);
void mcc_symtab_push_function_scope(mcc_symtab_t *symtab);
void mcc_symtab_pop_scope(mcc_symtab_t *symtab);
mcc_scope_t *mcc_symtab_current_scope(mcc_symtab_t *symtab);
bool mcc_symtab_is_global_scope(mcc_symtab_t *symtab);

/* Symbol operations */
mcc_symbol_t *mcc_symtab_define(mcc_symtab_t *symtab, const char *name,
                                 mcc_sym_kind_t kind, mcc_type_t *type,
                                 mcc_location_t loc);
mcc_symbol_t *mcc_symtab_lookup(mcc_symtab_t *symtab, const char *name);
mcc_symbol_t *mcc_symtab_lookup_current(mcc_symtab_t *symtab, const char *name);

/* Tag namespace operations */
mcc_symbol_t *mcc_symtab_define_tag(mcc_symtab_t *symtab, const char *name,
                                     mcc_sym_kind_t kind, mcc_type_t *type,
                                     mcc_location_t loc);
mcc_symbol_t *mcc_symtab_lookup_tag(mcc_symtab_t *symtab, const char *name);
mcc_symbol_t *mcc_symtab_lookup_tag_current(mcc_symtab_t *symtab, const char *name);

/* Label operations (function scope) */
mcc_symbol_t *mcc_symtab_define_label(mcc_symtab_t *symtab, const char *name,
                                       mcc_location_t loc);
mcc_symbol_t *mcc_symtab_lookup_label(mcc_symtab_t *symtab, const char *name);

/* Typedef check (for parser) */
bool mcc_symtab_is_typedef(mcc_symtab_t *symtab, const char *name);

/* Utilities */
const char *mcc_sym_kind_name(mcc_sym_kind_t kind);

#endif /* MCC_SYMTAB_H */
