/*
 * MCC - Micro C Compiler
 * Lexer - Identifier and keyword processing with C standard checks
 */

#include "lex_internal.h"

/* Keyword table with C standard requirements */
static const lex_keyword_entry_t keywords[] = {
    /* ============================================================
     * C89 Keywords (always available)
     * ============================================================ */
    {"auto",     TOK_AUTO,     MCC_FEAT_COUNT},
    {"break",    TOK_BREAK,    MCC_FEAT_COUNT},
    {"case",     TOK_CASE,     MCC_FEAT_COUNT},
    {"char",     TOK_CHAR,     MCC_FEAT_COUNT},
    {"const",    TOK_CONST,    MCC_FEAT_COUNT},
    {"continue", TOK_CONTINUE, MCC_FEAT_COUNT},
    {"default",  TOK_DEFAULT,  MCC_FEAT_COUNT},
    {"do",       TOK_DO,       MCC_FEAT_COUNT},
    {"double",   TOK_DOUBLE,   MCC_FEAT_COUNT},
    {"else",     TOK_ELSE,     MCC_FEAT_COUNT},
    {"enum",     TOK_ENUM,     MCC_FEAT_COUNT},
    {"extern",   TOK_EXTERN,   MCC_FEAT_COUNT},
    {"float",    TOK_FLOAT,    MCC_FEAT_COUNT},
    {"for",      TOK_FOR,      MCC_FEAT_COUNT},
    {"goto",     TOK_GOTO,     MCC_FEAT_COUNT},
    {"if",       TOK_IF,       MCC_FEAT_COUNT},
    {"int",      TOK_INT,      MCC_FEAT_COUNT},
    {"long",     TOK_LONG,     MCC_FEAT_COUNT},
    {"register", TOK_REGISTER, MCC_FEAT_COUNT},
    {"return",   TOK_RETURN,   MCC_FEAT_COUNT},
    {"short",    TOK_SHORT,    MCC_FEAT_COUNT},
    {"signed",   TOK_SIGNED,   MCC_FEAT_COUNT},
    {"sizeof",   TOK_SIZEOF,   MCC_FEAT_COUNT},
    {"static",   TOK_STATIC,   MCC_FEAT_COUNT},
    {"struct",   TOK_STRUCT,   MCC_FEAT_COUNT},
    {"switch",   TOK_SWITCH,   MCC_FEAT_COUNT},
    {"typedef",  TOK_TYPEDEF,  MCC_FEAT_COUNT},
    {"union",    TOK_UNION,    MCC_FEAT_COUNT},
    {"unsigned", TOK_UNSIGNED, MCC_FEAT_COUNT},
    {"void",     TOK_VOID,     MCC_FEAT_COUNT},
    {"volatile", TOK_VOLATILE, MCC_FEAT_COUNT},
    {"while",    TOK_WHILE,    MCC_FEAT_COUNT},
    
    /* ============================================================
     * C99 Keywords
     * ============================================================ */
    {"inline",     TOK_INLINE,     MCC_FEAT_INLINE},
    {"restrict",   TOK_RESTRICT,   MCC_FEAT_RESTRICT},
    {"_Bool",      TOK__BOOL,      MCC_FEAT_BOOL},
    {"_Complex",   TOK__COMPLEX,   MCC_FEAT_COMPLEX},
    {"_Imaginary", TOK__IMAGINARY, MCC_FEAT_IMAGINARY},
    
    /* ============================================================
     * C11 Keywords
     * ============================================================ */
    {"_Alignas",       TOK__ALIGNAS,       MCC_FEAT_ALIGNAS},
    {"_Alignof",       TOK__ALIGNOF,       MCC_FEAT_ALIGNOF},
    {"_Atomic",        TOK__ATOMIC,        MCC_FEAT_ATOMIC},
    {"_Generic",       TOK__GENERIC,       MCC_FEAT_GENERIC},
    {"_Noreturn",      TOK__NORETURN,      MCC_FEAT_NORETURN},
    {"_Static_assert", TOK__STATIC_ASSERT, MCC_FEAT_STATIC_ASSERT},
    {"_Thread_local",  TOK__THREAD_LOCAL,  MCC_FEAT_THREAD_LOCAL},
    
    /* ============================================================
     * C23 Keywords
     * ============================================================ */
    {"true",          TOK_TRUE,          MCC_FEAT_TRUE_FALSE},
    {"false",         TOK_FALSE,         MCC_FEAT_TRUE_FALSE},
    {"nullptr",       TOK_NULLPTR,       MCC_FEAT_NULLPTR},
    {"constexpr",     TOK_CONSTEXPR,     MCC_FEAT_CONSTEXPR},
    {"typeof",        TOK_TYPEOF,        MCC_FEAT_TYPEOF},
    {"typeof_unqual", TOK_TYPEOF_UNQUAL, MCC_FEAT_TYPEOF_UNQUAL},
    
    /* C23 alternative spellings (without underscore) - use BOOL_KEYWORD for bool */
    {"alignas",       TOK_ALIGNAS,       MCC_FEAT_ALIGNAS},
    {"alignof",       TOK_ALIGNOF,       MCC_FEAT_ALIGNOF},
    {"bool",          TOK_BOOL,          MCC_FEAT_BOOL_KEYWORD},
    {"static_assert", TOK_STATIC_ASSERT, MCC_FEAT_STATIC_ASSERT},
    {"thread_local",  TOK_THREAD_LOCAL,  MCC_FEAT_THREAD_LOCAL},
    
    {NULL, 0, MCC_FEAT_COUNT}
};

mcc_token_type_t lex_lookup_keyword(mcc_lexer_t *lex, const char *name, size_t len)
{
    for (int i = 0; keywords[i].name; i++) {
        if (strlen(keywords[i].name) == len &&
            strncmp(keywords[i].name, name, len) == 0) {
            
            /* Check if keyword requires a specific C standard feature */
            mcc_feature_id_t feat = keywords[i].required_feature;
            if (feat != MCC_FEAT_COUNT && !lex_has_feature(lex, feat)) {
                /* Keywords starting with underscore followed by uppercase are reserved
                 * in all C standards, so we should recognize them and let the parser
                 * give a proper error message about the required C standard */
                if (name[0] == '_' && len > 1 && isupper((unsigned char)name[1])) {
                    /* Reserved identifier - return as keyword so parser can give
                     * a clear error message about C standard requirements */
                    return keywords[i].type;
                }
                /* Other keywords (like 'inline', 'restrict', 'true', 'false', etc.)
                 * Emit a warning that this is a keyword in a newer C standard */
                const char *std_name = "C99";
                if (feat == MCC_FEAT_TRUE_FALSE || feat == MCC_FEAT_NULLPTR ||
                    feat == MCC_FEAT_CONSTEXPR || feat == MCC_FEAT_TYPEOF ||
                    feat == MCC_FEAT_TYPEOF_UNQUAL || feat == MCC_FEAT_BOOL_KEYWORD) {
                    std_name = "C23";
                } else if (feat == MCC_FEAT_ALIGNAS || feat == MCC_FEAT_ALIGNOF ||
                           feat == MCC_FEAT_ATOMIC || feat == MCC_FEAT_GENERIC ||
                           feat == MCC_FEAT_NORETURN || feat == MCC_FEAT_STATIC_ASSERT ||
                           feat == MCC_FEAT_THREAD_LOCAL) {
                    std_name = "C11";
                }
                mcc_location_t loc = {lex->filename, lex->line, lex->column};
                mcc_warning_at(lex->ctx, loc,
                               "'%s' is a keyword in %s; treating as identifier",
                               keywords[i].name, std_name);
                return TOK_IDENT;
            }
            
            return keywords[i].type;
        }
    }
    return TOK_IDENT;
}

mcc_token_t *lex_identifier(mcc_lexer_t *lex)
{
    size_t start = lex->pos;
    int start_col = lex->column;
    
    while (isalnum(lex->current) || lex->current == '_') {
        lex_advance(lex);
    }
    
    size_t len = lex->pos - start;
    
    /* Check for string/char literal prefixes (C11/C23) */
    if (lex->current == '\'' || lex->current == '"') {
        const char *prefix = lex->source + start;
        bool is_char = (lex->current == '\'');
        
        /* u8 prefix (C11 for strings, C23 for chars) */
        if (len == 2 && prefix[0] == 'u' && prefix[1] == '8') {
            if (is_char) {
                return lex_char_literal(lex);
            } else {
                return lex_string_literal(lex);
            }
        }
        /* u, U, L prefixes (C11) */
        if (len == 1 && (prefix[0] == 'u' || prefix[0] == 'U' || prefix[0] == 'L')) {
            if (is_char) {
                return lex_char_literal(lex);
            } else {
                return lex_string_literal(lex);
            }
        }
    }
    
    mcc_token_type_t type = lex_lookup_keyword(lex, lex->source + start, len);
    
    mcc_token_t *tok = lex_make_token(lex, type);
    tok->location.column = start_col;
    tok->text = mcc_alloc(lex->ctx, len + 1);
    memcpy((char*)tok->text, lex->source + start, len);
    ((char*)tok->text)[len] = '\0';
    tok->text_len = len;
    
    return tok;
}
