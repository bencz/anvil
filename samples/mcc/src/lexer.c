/*
 * MCC - Micro C Compiler
 * Lexer implementation
 */

#include "mcc.h"

/* Keyword table */
static struct {
    const char *name;
    mcc_token_type_t type;
} keywords[] = {
    {"auto",     TOK_AUTO},
    {"break",    TOK_BREAK},
    {"case",     TOK_CASE},
    {"char",     TOK_CHAR},
    {"const",    TOK_CONST},
    {"continue", TOK_CONTINUE},
    {"default",  TOK_DEFAULT},
    {"do",       TOK_DO},
    {"double",   TOK_DOUBLE},
    {"else",     TOK_ELSE},
    {"enum",     TOK_ENUM},
    {"extern",   TOK_EXTERN},
    {"float",    TOK_FLOAT},
    {"for",      TOK_FOR},
    {"goto",     TOK_GOTO},
    {"if",       TOK_IF},
    {"int",      TOK_INT},
    {"long",     TOK_LONG},
    {"register", TOK_REGISTER},
    {"return",   TOK_RETURN},
    {"short",    TOK_SHORT},
    {"signed",   TOK_SIGNED},
    {"sizeof",   TOK_SIZEOF},
    {"static",   TOK_STATIC},
    {"struct",   TOK_STRUCT},
    {"switch",   TOK_SWITCH},
    {"typedef",  TOK_TYPEDEF},
    {"union",    TOK_UNION},
    {"unsigned", TOK_UNSIGNED},
    {"void",     TOK_VOID},
    {"volatile", TOK_VOLATILE},
    {"while",    TOK_WHILE},
    {NULL, 0}
};

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
    return type >= TOK_AUTO && type <= TOK_SIZEOF;
}

bool mcc_token_is_type_specifier(mcc_token_type_t type)
{
    switch (type) {
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
            return true;
        default:
            return false;
    }
}

bool mcc_token_is_type_qualifier(mcc_token_type_t type)
{
    return type == TOK_CONST || type == TOK_VOLATILE;
}

bool mcc_token_is_storage_class(mcc_token_type_t type)
{
    switch (type) {
        case TOK_AUTO:
        case TOK_REGISTER:
        case TOK_STATIC:
        case TOK_EXTERN:
        case TOK_TYPEDEF:
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

/* Lexer implementation */
mcc_lexer_t *mcc_lexer_create(mcc_context_t *ctx)
{
    mcc_lexer_t *lex = mcc_alloc(ctx, sizeof(mcc_lexer_t));
    if (!lex) return NULL;
    lex->ctx = ctx;
    lex->line = 1;
    lex->column = 1;
    lex->at_bol = true;
    return lex;
}

void mcc_lexer_destroy(mcc_lexer_t *lex)
{
    (void)lex; /* Arena allocated */
}

void mcc_lexer_init_string(mcc_lexer_t *lex, const char *source, const char *filename)
{
    lex->source = source;
    lex->source_len = strlen(source);
    lex->pos = 0;
    lex->filename = filename;
    lex->line = 1;
    lex->column = 1;
    lex->current = source[0];
    lex->at_bol = true;
    lex->has_space = false;
    lex->peek_token = NULL;
}

void mcc_lexer_init_file(mcc_lexer_t *lex, const char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f) {
        mcc_fatal(lex->ctx, "Cannot open file: %s", filename);
        return;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *buf = mcc_alloc(lex->ctx, size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    
    mcc_lexer_init_string(lex, buf, filename);
}

static int lex_peek(mcc_lexer_t *lex)
{
    if (lex->pos >= lex->source_len) return '\0';
    return lex->source[lex->pos];
}

static int lex_peek_next(mcc_lexer_t *lex)
{
    if (lex->pos + 1 >= lex->source_len) return '\0';
    return lex->source[lex->pos + 1];
}

static int lex_advance(mcc_lexer_t *lex)
{
    int c = lex->current;
    if (lex->pos < lex->source_len) {
        lex->pos++;
        lex->column++;
        if (c == '\n') {
            lex->line++;
            lex->column = 1;
            lex->at_bol = true;
        }
        lex->current = lex_peek(lex);
    }
    return c;
}

static void lex_skip_whitespace(mcc_lexer_t *lex)
{
    while (lex->current == ' ' || lex->current == '\t' ||
           lex->current == '\r' || lex->current == '\f' ||
           lex->current == '\v') {
        lex->has_space = true;
        lex_advance(lex);
    }
}

static void lex_skip_line_comment(mcc_lexer_t *lex)
{
    /* Skip // comment (C99, but common extension) */
    while (lex->current && lex->current != '\n') {
        lex_advance(lex);
    }
}

static void lex_skip_block_comment(mcc_lexer_t *lex)
{
    /* Skip /* comment */
    lex_advance(lex); /* Skip * */
    while (lex->current) {
        if (lex->current == '*' && lex_peek_next(lex) == '/') {
            lex_advance(lex);
            lex_advance(lex);
            return;
        }
        lex_advance(lex);
    }
    mcc_error(lex->ctx, "Unterminated block comment");
}

static mcc_token_t *lex_make_token(mcc_lexer_t *lex, mcc_token_type_t type)
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

static mcc_token_type_t lex_lookup_keyword(const char *name, size_t len)
{
    for (int i = 0; keywords[i].name; i++) {
        if (strlen(keywords[i].name) == len &&
            strncmp(keywords[i].name, name, len) == 0) {
            return keywords[i].type;
        }
    }
    return TOK_IDENT;
}

static mcc_token_t *lex_identifier(mcc_lexer_t *lex)
{
    size_t start = lex->pos;
    int start_col = lex->column;
    
    while (isalnum(lex->current) || lex->current == '_') {
        lex_advance(lex);
    }
    
    size_t len = lex->pos - start;
    mcc_token_type_t type = lex_lookup_keyword(lex->source + start, len);
    
    mcc_token_t *tok = lex_make_token(lex, type);
    tok->location.column = start_col;
    tok->text = mcc_alloc(lex->ctx, len + 1);
    memcpy((char*)tok->text, lex->source + start, len);
    ((char*)tok->text)[len] = '\0';
    tok->text_len = len;
    
    return tok;
}

static mcc_token_t *lex_number(mcc_lexer_t *lex)
{
    size_t start = lex->pos;
    int start_col = lex->column;
    bool is_float = false;
    int base = 10;
    
    /* Check for hex/octal prefix */
    if (lex->current == '0') {
        lex_advance(lex);
        if (lex->current == 'x' || lex->current == 'X') {
            base = 16;
            lex_advance(lex);
        } else if (isdigit(lex->current)) {
            base = 8;
        }
    }
    
    /* Read digits */
    while (1) {
        if (base == 16 && isxdigit(lex->current)) {
            lex_advance(lex);
        } else if (isdigit(lex->current)) {
            lex_advance(lex);
        } else {
            break;
        }
    }
    
    /* Check for decimal point */
    if (lex->current == '.' && base == 10) {
        is_float = true;
        lex_advance(lex);
        while (isdigit(lex->current)) {
            lex_advance(lex);
        }
    }
    
    /* Check for exponent */
    if ((lex->current == 'e' || lex->current == 'E') && base == 10) {
        is_float = true;
        lex_advance(lex);
        if (lex->current == '+' || lex->current == '-') {
            lex_advance(lex);
        }
        while (isdigit(lex->current)) {
            lex_advance(lex);
        }
    }
    
    /* Read suffix */
    mcc_int_suffix_t int_suffix = INT_SUFFIX_NONE;
    mcc_float_suffix_t float_suffix = FLOAT_SUFFIX_NONE;
    
    if (is_float) {
        if (lex->current == 'f' || lex->current == 'F') {
            float_suffix = FLOAT_SUFFIX_F;
            lex_advance(lex);
        } else if (lex->current == 'l' || lex->current == 'L') {
            float_suffix = FLOAT_SUFFIX_L;
            lex_advance(lex);
        }
    } else {
        bool has_u = false, has_l = false, has_ll = false;
        while (1) {
            if ((lex->current == 'u' || lex->current == 'U') && !has_u) {
                has_u = true;
                lex_advance(lex);
            } else if ((lex->current == 'l' || lex->current == 'L') && !has_ll) {
                if (has_l) {
                    has_ll = true;
                    has_l = false;
                } else {
                    has_l = true;
                }
                lex_advance(lex);
            } else {
                break;
            }
        }
        if (has_u && has_ll) int_suffix = INT_SUFFIX_ULL;
        else if (has_ll) int_suffix = INT_SUFFIX_LL;
        else if (has_u && has_l) int_suffix = INT_SUFFIX_UL;
        else if (has_l) int_suffix = INT_SUFFIX_L;
        else if (has_u) int_suffix = INT_SUFFIX_U;
    }
    
    size_t len = lex->pos - start;
    char *text = mcc_alloc(lex->ctx, len + 1);
    memcpy(text, lex->source + start, len);
    text[len] = '\0';
    
    mcc_token_t *tok = lex_make_token(lex, is_float ? TOK_FLOAT_LIT : TOK_INT_LIT);
    tok->location.column = start_col;
    tok->text = text;
    tok->text_len = len;
    
    if (is_float) {
        tok->literal.float_val.value = strtod(text, NULL);
        tok->literal.float_val.suffix = float_suffix;
    } else {
        tok->literal.int_val.value = strtoull(text, NULL, base);
        tok->literal.int_val.suffix = int_suffix;
    }
    
    return tok;
}

static int lex_escape_char(mcc_lexer_t *lex)
{
    lex_advance(lex); /* Skip backslash */
    int c = lex->current;
    lex_advance(lex);
    
    switch (c) {
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'n':  return '\n';
        case 'r':  return '\r';
        case 't':  return '\t';
        case 'v':  return '\v';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case '?':  return '?';
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            /* Octal escape */
            int val = c - '0';
            for (int i = 0; i < 2 && lex->current >= '0' && lex->current <= '7'; i++) {
                val = val * 8 + (lex->current - '0');
                lex_advance(lex);
            }
            return val;
        }
        case 'x': {
            /* Hex escape */
            int val = 0;
            while (isxdigit(lex->current)) {
                int d = lex->current;
                if (d >= '0' && d <= '9') d -= '0';
                else if (d >= 'a' && d <= 'f') d = d - 'a' + 10;
                else d = d - 'A' + 10;
                val = val * 16 + d;
                lex_advance(lex);
            }
            return val;
        }
        default:
            mcc_warning(lex->ctx, "Unknown escape sequence '\\%c'", c);
            return c;
    }
}

static mcc_token_t *lex_char_literal(mcc_lexer_t *lex)
{
    int start_col = lex->column;
    lex_advance(lex); /* Skip opening quote */
    
    int value;
    if (lex->current == '\\') {
        value = lex_escape_char(lex);
    } else {
        value = lex->current;
        lex_advance(lex);
    }
    
    if (lex->current != '\'') {
        mcc_error(lex->ctx, "Unterminated character literal");
    } else {
        lex_advance(lex);
    }
    
    mcc_token_t *tok = lex_make_token(lex, TOK_CHAR_LIT);
    tok->location.column = start_col;
    tok->literal.char_val.value = value;
    return tok;
}

static mcc_token_t *lex_string_literal(mcc_lexer_t *lex)
{
    int start_col = lex->column;
    lex_advance(lex); /* Skip opening quote */
    
    char buf[MCC_MAX_STRING_LEN];
    size_t len = 0;
    
    while (lex->current && lex->current != '"' && lex->current != '\n') {
        int c;
        if (lex->current == '\\') {
            c = lex_escape_char(lex);
        } else {
            c = lex->current;
            lex_advance(lex);
        }
        if (len < MCC_MAX_STRING_LEN - 1) {
            buf[len++] = (char)c;
        }
    }
    buf[len] = '\0';
    
    if (lex->current != '"') {
        mcc_error(lex->ctx, "Unterminated string literal");
    } else {
        lex_advance(lex);
    }
    
    mcc_token_t *tok = lex_make_token(lex, TOK_STRING_LIT);
    tok->location.column = start_col;
    tok->literal.string_val.value = mcc_strdup(lex->ctx, buf);
    tok->literal.string_val.length = len;
    tok->text = tok->literal.string_val.value;
    tok->text_len = len;
    return tok;
}

mcc_token_t *mcc_lexer_next(mcc_lexer_t *lex)
{
    /* Return peeked token if available */
    if (lex->peek_token) {
        mcc_token_t *tok = lex->peek_token;
        lex->peek_token = NULL;
        return tok;
    }
    
again:
    lex->has_space = false;
    lex_skip_whitespace(lex);
    
    /* Handle newlines (for preprocessor) */
    if (lex->current == '\n') {
        mcc_token_t *tok = lex_make_token(lex, TOK_NEWLINE);
        lex_advance(lex);
        return tok;
    }
    
    /* EOF */
    if (lex->current == '\0') {
        return lex_make_token(lex, TOK_EOF);
    }
    
    /* Comments */
    if (lex->current == '/') {
        if (lex_peek_next(lex) == '/') {
            lex_advance(lex);
            lex_advance(lex);
            lex_skip_line_comment(lex);
            lex->has_space = true;
            goto again;
        }
        if (lex_peek_next(lex) == '*') {
            lex_advance(lex);
            lex_skip_block_comment(lex);
            lex->has_space = true;
            goto again;
        }
    }
    
    /* Identifiers and keywords */
    if (isalpha(lex->current) || lex->current == '_') {
        return lex_identifier(lex);
    }
    
    /* Numbers */
    if (isdigit(lex->current)) {
        return lex_number(lex);
    }
    
    /* Character literal */
    if (lex->current == '\'') {
        return lex_char_literal(lex);
    }
    
    /* String literal */
    if (lex->current == '"') {
        return lex_string_literal(lex);
    }
    
    /* Operators and punctuation */
    int c = lex->current;
    int start_col = lex->column;
    lex_advance(lex);
    
    mcc_token_t *tok;
    
    switch (c) {
        case '+':
            if (lex->current == '+') { lex_advance(lex); tok = lex_make_token(lex, TOK_INC); }
            else if (lex->current == '=') { lex_advance(lex); tok = lex_make_token(lex, TOK_PLUS_ASSIGN); }
            else tok = lex_make_token(lex, TOK_PLUS);
            break;
        case '-':
            if (lex->current == '-') { lex_advance(lex); tok = lex_make_token(lex, TOK_DEC); }
            else if (lex->current == '=') { lex_advance(lex); tok = lex_make_token(lex, TOK_MINUS_ASSIGN); }
            else if (lex->current == '>') { lex_advance(lex); tok = lex_make_token(lex, TOK_ARROW); }
            else tok = lex_make_token(lex, TOK_MINUS);
            break;
        case '*':
            if (lex->current == '=') { lex_advance(lex); tok = lex_make_token(lex, TOK_STAR_ASSIGN); }
            else tok = lex_make_token(lex, TOK_STAR);
            break;
        case '/':
            if (lex->current == '=') { lex_advance(lex); tok = lex_make_token(lex, TOK_SLASH_ASSIGN); }
            else tok = lex_make_token(lex, TOK_SLASH);
            break;
        case '%':
            if (lex->current == '=') { lex_advance(lex); tok = lex_make_token(lex, TOK_PERCENT_ASSIGN); }
            else tok = lex_make_token(lex, TOK_PERCENT);
            break;
        case '=':
            if (lex->current == '=') { lex_advance(lex); tok = lex_make_token(lex, TOK_EQ); }
            else tok = lex_make_token(lex, TOK_ASSIGN);
            break;
        case '!':
            if (lex->current == '=') { lex_advance(lex); tok = lex_make_token(lex, TOK_NE); }
            else tok = lex_make_token(lex, TOK_NOT);
            break;
        case '<':
            if (lex->current == '<') {
                lex_advance(lex);
                if (lex->current == '=') { lex_advance(lex); tok = lex_make_token(lex, TOK_LSHIFT_ASSIGN); }
                else tok = lex_make_token(lex, TOK_LSHIFT);
            }
            else if (lex->current == '=') { lex_advance(lex); tok = lex_make_token(lex, TOK_LE); }
            else tok = lex_make_token(lex, TOK_LT);
            break;
        case '>':
            if (lex->current == '>') {
                lex_advance(lex);
                if (lex->current == '=') { lex_advance(lex); tok = lex_make_token(lex, TOK_RSHIFT_ASSIGN); }
                else tok = lex_make_token(lex, TOK_RSHIFT);
            }
            else if (lex->current == '=') { lex_advance(lex); tok = lex_make_token(lex, TOK_GE); }
            else tok = lex_make_token(lex, TOK_GT);
            break;
        case '&':
            if (lex->current == '&') { lex_advance(lex); tok = lex_make_token(lex, TOK_AND); }
            else if (lex->current == '=') { lex_advance(lex); tok = lex_make_token(lex, TOK_AMP_ASSIGN); }
            else tok = lex_make_token(lex, TOK_AMP);
            break;
        case '|':
            if (lex->current == '|') { lex_advance(lex); tok = lex_make_token(lex, TOK_OR); }
            else if (lex->current == '=') { lex_advance(lex); tok = lex_make_token(lex, TOK_PIPE_ASSIGN); }
            else tok = lex_make_token(lex, TOK_PIPE);
            break;
        case '^':
            if (lex->current == '=') { lex_advance(lex); tok = lex_make_token(lex, TOK_CARET_ASSIGN); }
            else tok = lex_make_token(lex, TOK_CARET);
            break;
        case '~':
            tok = lex_make_token(lex, TOK_TILDE);
            break;
        case '?':
            tok = lex_make_token(lex, TOK_QUESTION);
            break;
        case ':':
            tok = lex_make_token(lex, TOK_COLON);
            break;
        case ';':
            tok = lex_make_token(lex, TOK_SEMICOLON);
            break;
        case ',':
            tok = lex_make_token(lex, TOK_COMMA);
            break;
        case '(':
            tok = lex_make_token(lex, TOK_LPAREN);
            break;
        case ')':
            tok = lex_make_token(lex, TOK_RPAREN);
            break;
        case '[':
            tok = lex_make_token(lex, TOK_LBRACKET);
            break;
        case ']':
            tok = lex_make_token(lex, TOK_RBRACKET);
            break;
        case '{':
            tok = lex_make_token(lex, TOK_LBRACE);
            break;
        case '}':
            tok = lex_make_token(lex, TOK_RBRACE);
            break;
        case '#':
            if (lex->current == '#') { lex_advance(lex); tok = lex_make_token(lex, TOK_HASH_HASH); }
            else tok = lex_make_token(lex, TOK_HASH);
            break;
        case '.':
            if (lex->current == '.' && lex_peek_next(lex) == '.') {
                lex_advance(lex);
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_ELLIPSIS);
            } else if (isdigit(lex->current)) {
                /* Float starting with . */
                lex->pos--;
                lex->column--;
                lex->current = '.';
                return lex_number(lex);
            } else {
                tok = lex_make_token(lex, TOK_DOT);
            }
            break;
        default:
            mcc_error(lex->ctx, "Unexpected character '%c' (0x%02x)", c, c);
            goto again;
    }
    
    tok->location.column = start_col;
    return tok;
}

mcc_token_t *mcc_lexer_peek(mcc_lexer_t *lex)
{
    if (!lex->peek_token) {
        lex->peek_token = mcc_lexer_next(lex);
    }
    return lex->peek_token;
}

bool mcc_lexer_match(mcc_lexer_t *lex, mcc_token_type_t type)
{
    if (mcc_lexer_peek(lex)->type == type) {
        mcc_lexer_next(lex);
        return true;
    }
    return false;
}

bool mcc_lexer_check(mcc_lexer_t *lex, mcc_token_type_t type)
{
    return mcc_lexer_peek(lex)->type == type;
}

mcc_token_t *mcc_lexer_expect(mcc_lexer_t *lex, mcc_token_type_t type, const char *msg)
{
    mcc_token_t *tok = mcc_lexer_next(lex);
    if (tok->type != type) {
        mcc_error_at(lex->ctx, tok->location, "Expected %s, got '%s'",
                     msg ? msg : mcc_token_type_name(type),
                     mcc_token_to_string(tok));
    }
    return tok;
}

mcc_location_t mcc_lexer_location(mcc_lexer_t *lex)
{
    return (mcc_location_t){lex->filename, lex->line, lex->column};
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
