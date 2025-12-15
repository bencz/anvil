/*
 * MCC - Micro C Compiler
 * Lexer interface
 */

#ifndef MCC_LEXER_H
#define MCC_LEXER_H

/* Forward declaration */
typedef struct mcc_lexer mcc_lexer_t;

/* Lexer structure */
struct mcc_lexer {
    mcc_context_t *ctx;
    
    /* Input source */
    const char *source;         /* Source buffer */
    size_t source_len;          /* Source length */
    size_t pos;                 /* Current position */
    
    /* Location tracking */
    const char *filename;
    int line;
    int column;
    
    /* Current character */
    int current;
    
    /* Peek buffer (for lookahead) */
    mcc_token_t *peek_token;
    
    /* Token at beginning of line? */
    bool at_bol;
    
    /* Has whitespace before current token? */
    bool has_space;
};

/* Lexer lifecycle */
mcc_lexer_t *mcc_lexer_create(mcc_context_t *ctx);
void mcc_lexer_destroy(mcc_lexer_t *lex);

/* Initialize with source */
void mcc_lexer_init_string(mcc_lexer_t *lex, const char *source, const char *filename);
void mcc_lexer_init_file(mcc_lexer_t *lex, const char *filename);

/* Token retrieval */
mcc_token_t *mcc_lexer_next(mcc_lexer_t *lex);
mcc_token_t *mcc_lexer_peek(mcc_lexer_t *lex);

/* Token matching */
bool mcc_lexer_match(mcc_lexer_t *lex, mcc_token_type_t type);
bool mcc_lexer_check(mcc_lexer_t *lex, mcc_token_type_t type);
mcc_token_t *mcc_lexer_expect(mcc_lexer_t *lex, mcc_token_type_t type, const char *msg);

/* Location */
mcc_location_t mcc_lexer_location(mcc_lexer_t *lex);

/* Token allocation */
mcc_token_t *mcc_token_create(mcc_context_t *ctx);
mcc_token_t *mcc_token_copy(mcc_context_t *ctx, mcc_token_t *tok);
void mcc_token_list_free(mcc_token_t *list);

#endif /* MCC_LEXER_H */
