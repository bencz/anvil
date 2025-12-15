/*
 * MCC - Micro C Compiler
 * Parser interface
 */

#ifndef MCC_PARSER_H
#define MCC_PARSER_H

/* Forward declarations */
typedef struct mcc_parser mcc_parser_t;
struct mcc_type;

/* Struct/union type entry */
typedef struct mcc_struct_entry {
    const char *tag;
    struct mcc_type *type;
    struct mcc_struct_entry *next;
} mcc_struct_entry_t;

/* Typedef entry */
typedef struct mcc_typedef_entry {
    const char *name;
    struct mcc_type *type;
    struct mcc_typedef_entry *next;
} mcc_typedef_entry_t;

/* Parser state */
struct mcc_parser {
    mcc_context_t *ctx;
    mcc_preprocessor_t *pp;     /* Token source */
    
    /* Current and lookahead tokens */
    mcc_token_t *current;
    mcc_token_t *peek;
    
    /* Scope tracking for typedef names */
    struct mcc_symtab *symtab;
    
    /* Struct/union type table */
    mcc_struct_entry_t *struct_types;
    
    /* Typedef table */
    mcc_typedef_entry_t *typedefs;
    
    /* Error recovery */
    bool panic_mode;
    int sync_depth;
};

/* Parser lifecycle */
mcc_parser_t *mcc_parser_create(mcc_context_t *ctx, mcc_preprocessor_t *pp);
void mcc_parser_destroy(mcc_parser_t *parser);

/* Main parsing entry point */
mcc_ast_node_t *mcc_parser_parse(mcc_parser_t *parser);

/* Parse specific constructs (for testing) */
mcc_ast_node_t *mcc_parser_parse_expression(mcc_parser_t *parser);
mcc_ast_node_t *mcc_parser_parse_statement(mcc_parser_t *parser);
mcc_ast_node_t *mcc_parser_parse_declaration(mcc_parser_t *parser);

/* Token operations */
mcc_token_t *mcc_parser_advance(mcc_parser_t *parser);
bool mcc_parser_check(mcc_parser_t *parser, mcc_token_type_t type);
bool mcc_parser_match(mcc_parser_t *parser, mcc_token_type_t type);
mcc_token_t *mcc_parser_expect(mcc_parser_t *parser, mcc_token_type_t type, const char *msg);

/* Error recovery */
void mcc_parser_synchronize(mcc_parser_t *parser);

#endif /* MCC_PARSER_H */
