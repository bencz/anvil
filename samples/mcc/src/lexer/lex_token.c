/*
 * MCC - Micro C Compiler
 * Lexer - Token creation and utilities
 */

#include "lex_internal.h"

/* Token type names */
static const char *token_names[] = {
    [TOK_EOF]           = "EOF",
    [TOK_IDENT]         = "identifier",
    [TOK_INT_LIT]       = "integer",
    [TOK_FLOAT_LIT]     = "float",
    [TOK_CHAR_LIT]      = "character",
    [TOK_STRING_LIT]    = "string",
    [TOK_AUTO]          = "auto",
    [TOK_REGISTER]      = "register",
    [TOK_STATIC]        = "static",
    [TOK_EXTERN]        = "extern",
    [TOK_TYPEDEF]       = "typedef",
    [TOK_VOID]          = "void",
    [TOK_CHAR]          = "char",
    [TOK_SHORT]         = "short",
    [TOK_INT]           = "int",
    [TOK_LONG]          = "long",
    [TOK_FLOAT]         = "float",
    [TOK_DOUBLE]        = "double",
    [TOK_SIGNED]        = "signed",
    [TOK_UNSIGNED]      = "unsigned",
    [TOK_STRUCT]        = "struct",
    [TOK_UNION]         = "union",
    [TOK_ENUM]          = "enum",
    [TOK_CONST]         = "const",
    [TOK_VOLATILE]      = "volatile",
    [TOK_IF]            = "if",
    [TOK_ELSE]          = "else",
    [TOK_SWITCH]        = "switch",
    [TOK_CASE]          = "case",
    [TOK_DEFAULT]       = "default",
    [TOK_WHILE]         = "while",
    [TOK_DO]            = "do",
    [TOK_FOR]           = "for",
    [TOK_GOTO]          = "goto",
    [TOK_CONTINUE]      = "continue",
    [TOK_BREAK]         = "break",
    [TOK_RETURN]        = "return",
    [TOK_SIZEOF]        = "sizeof",
    
    /* C99 Keywords */
    [TOK_INLINE]        = "inline",
    [TOK_RESTRICT]      = "restrict",
    [TOK__BOOL]         = "_Bool",
    [TOK__COMPLEX]      = "_Complex",
    [TOK__IMAGINARY]    = "_Imaginary",
    
    /* C11 Keywords */
    [TOK__ALIGNAS]      = "_Alignas",
    [TOK__ALIGNOF]      = "_Alignof",
    [TOK__ATOMIC]       = "_Atomic",
    [TOK__GENERIC]      = "_Generic",
    [TOK__NORETURN]     = "_Noreturn",
    [TOK__STATIC_ASSERT]= "_Static_assert",
    [TOK__THREAD_LOCAL] = "_Thread_local",
    
    /* C23 Keywords */
    [TOK_TRUE]          = "true",
    [TOK_FALSE]         = "false",
    [TOK_NULLPTR]       = "nullptr",
    [TOK_CONSTEXPR]     = "constexpr",
    [TOK_TYPEOF]        = "typeof",
    [TOK_TYPEOF_UNQUAL] = "typeof_unqual",
    [TOK__BITINT]       = "_BitInt",
    [TOK_ALIGNAS]       = "alignas",
    [TOK_ALIGNOF]       = "alignof",
    [TOK_BOOL]          = "bool",
    [TOK_STATIC_ASSERT] = "static_assert",
    [TOK_THREAD_LOCAL]  = "thread_local",
    
    [TOK_PLUS]          = "+",
    [TOK_MINUS]         = "-",
    [TOK_STAR]          = "*",
    [TOK_SLASH]         = "/",
    [TOK_PERCENT]       = "%",
    [TOK_EQ]            = "==",
    [TOK_NE]            = "!=",
    [TOK_LT]            = "<",
    [TOK_GT]            = ">",
    [TOK_LE]            = "<=",
    [TOK_GE]            = ">=",
    [TOK_AND]           = "&&",
    [TOK_OR]            = "||",
    [TOK_NOT]           = "!",
    [TOK_AMP]           = "&",
    [TOK_PIPE]          = "|",
    [TOK_CARET]         = "^",
    [TOK_TILDE]         = "~",
    [TOK_LSHIFT]        = "<<",
    [TOK_RSHIFT]        = ">>",
    [TOK_ASSIGN]        = "=",
    [TOK_PLUS_ASSIGN]   = "+=",
    [TOK_MINUS_ASSIGN]  = "-=",
    [TOK_STAR_ASSIGN]   = "*=",
    [TOK_SLASH_ASSIGN]  = "/=",
    [TOK_PERCENT_ASSIGN]= "%=",
    [TOK_AMP_ASSIGN]    = "&=",
    [TOK_PIPE_ASSIGN]   = "|=",
    [TOK_CARET_ASSIGN]  = "^=",
    [TOK_LSHIFT_ASSIGN] = "<<=",
    [TOK_RSHIFT_ASSIGN] = ">>=",
    [TOK_INC]           = "++",
    [TOK_DEC]           = "--",
    [TOK_ARROW]         = "->",
    [TOK_DOT]           = ".",
    [TOK_QUESTION]      = "?",
    [TOK_COLON]         = ":",
    [TOK_COMMA]         = ",",
    [TOK_SEMICOLON]     = ";",
    [TOK_LPAREN]        = "(",
    [TOK_RPAREN]        = ")",
    [TOK_LBRACKET]      = "[",
    [TOK_RBRACKET]      = "]",
    [TOK_LBRACE]        = "{",
    [TOK_RBRACE]        = "}",
    [TOK_HASH]          = "#",
    [TOK_HASH_HASH]     = "##",
    [TOK_ELLIPSIS]      = "...",
    [TOK_NEWLINE]       = "newline",
};

const char *mcc_token_type_name(mcc_token_type_t type)
{
    if (type < TOK_COUNT && token_names[type]) {
        return token_names[type];
    }
    return "unknown";
}

const char *mcc_token_to_string(mcc_token_t *tok)
{
    if (!tok) return "";
    if (tok->text) return tok->text;
    return mcc_token_type_name(tok->type);
}

bool mcc_token_is_keyword(mcc_token_type_t type)
{
    /* C89 keywords */
    if (type >= TOK_AUTO && type <= TOK_SIZEOF) return true;
    /* C99 keywords */
    if (type >= TOK_INLINE && type <= TOK__IMAGINARY) return true;
    /* C11 keywords */
    if (type >= TOK__ALIGNAS && type <= TOK__THREAD_LOCAL) return true;
    /* C23 keywords */
    if (type >= TOK_TRUE && type <= TOK_THREAD_LOCAL) return true;
    return false;
}

bool mcc_token_is_type_specifier(mcc_token_type_t type)
{
    switch (type) {
        /* C89 type specifiers */
        case TOK_VOID:
        case TOK_CHAR:
        case TOK_SHORT:
        case TOK_INT:
        case TOK_LONG:
        case TOK_FLOAT:
        case TOK_DOUBLE:
        case TOK_SIGNED:
        case TOK_UNSIGNED:
        case TOK_STRUCT:
        case TOK_UNION:
        case TOK_ENUM:
        /* C99 type specifiers */
        case TOK__BOOL:
        case TOK__COMPLEX:
        case TOK__IMAGINARY:
        /* C23 type specifiers */
        case TOK_BOOL:
        case TOK__BITINT:
        case TOK_TYPEOF:
        case TOK_TYPEOF_UNQUAL:
            return true;
        default:
            return false;
    }
}

bool mcc_token_is_type_qualifier(mcc_token_type_t type)
{
    switch (type) {
        /* C89 type qualifiers */
        case TOK_CONST:
        case TOK_VOLATILE:
        /* C99 type qualifiers */
        case TOK_RESTRICT:
        /* C11 type qualifiers */
        case TOK__ATOMIC:
            return true;
        default:
            return false;
    }
}

bool mcc_token_is_storage_class(mcc_token_type_t type)
{
    switch (type) {
        /* C89 storage class specifiers */
        case TOK_AUTO:
        case TOK_REGISTER:
        case TOK_STATIC:
        case TOK_EXTERN:
        case TOK_TYPEDEF:
        /* C11 storage class specifiers */
        case TOK__THREAD_LOCAL:
        case TOK_THREAD_LOCAL:
        /* C23 storage class specifiers */
        case TOK_CONSTEXPR:
            return true;
        default:
            return false;
    }
}

bool mcc_token_is_assignment_op(mcc_token_type_t type)
{
    switch (type) {
        case TOK_ASSIGN:
        case TOK_PLUS_ASSIGN:
        case TOK_MINUS_ASSIGN:
        case TOK_STAR_ASSIGN:
        case TOK_SLASH_ASSIGN:
        case TOK_PERCENT_ASSIGN:
        case TOK_AMP_ASSIGN:
        case TOK_PIPE_ASSIGN:
        case TOK_CARET_ASSIGN:
        case TOK_LSHIFT_ASSIGN:
        case TOK_RSHIFT_ASSIGN:
            return true;
        default:
            return false;
    }
}

bool mcc_token_is_comparison_op(mcc_token_type_t type)
{
    switch (type) {
        case TOK_EQ:
        case TOK_NE:
        case TOK_LT:
        case TOK_GT:
        case TOK_LE:
        case TOK_GE:
            return true;
        default:
            return false;
    }
}

bool mcc_token_is_unary_op(mcc_token_type_t type)
{
    switch (type) {
        case TOK_PLUS:
        case TOK_MINUS:
        case TOK_NOT:
        case TOK_TILDE:
        case TOK_STAR:
        case TOK_AMP:
        case TOK_INC:
        case TOK_DEC:
            return true;
        default:
            return false;
    }
}

mcc_token_t *lex_make_token(mcc_lexer_t *lex, mcc_token_type_t type)
{
    mcc_token_t *tok = mcc_alloc(lex->ctx, sizeof(mcc_token_t));
    tok->type = type;
    tok->location.filename = lex->filename;
    tok->location.line = lex->line;
    tok->location.column = lex->column;
    tok->at_bol = lex->at_bol;
    tok->has_space = lex->has_space;
    lex->at_bol = false;
    lex->has_space = false;
    return tok;
}

mcc_token_t *mcc_token_create(mcc_context_t *ctx)
{
    return mcc_alloc(ctx, sizeof(mcc_token_t));
}

mcc_token_t *mcc_token_copy(mcc_context_t *ctx, mcc_token_t *tok)
{
    mcc_token_t *copy = mcc_token_create(ctx);
    *copy = *tok;
    copy->next = NULL;
    if (tok->text) {
        copy->text = mcc_strdup(ctx, tok->text);
    }
    return copy;
}

void mcc_token_list_free(mcc_token_t *list)
{
    (void)list; /* Arena allocated */
}
