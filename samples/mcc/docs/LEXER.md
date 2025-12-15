# MCC Lexer Documentation

This document describes the lexer (tokenizer) component of MCC.

## Overview

The lexer converts source code text into a stream of tokens. Each token represents a meaningful unit of the language: keywords, identifiers, literals, operators, and punctuation.

## Token Types

### Keywords (32 C89 keywords)

```c
auto     break    case     char     const    continue
default  do       double   else     enum     extern
float    for      goto     if       int      long
register return   short    signed   sizeof   static
struct   switch   typedef  union    unsigned void
volatile while
```

### Identifiers

Identifiers start with a letter or underscore, followed by letters, digits, or underscores:

```
[a-zA-Z_][a-zA-Z0-9_]*
```

### Integer Literals

| Format | Example | Description |
|--------|---------|-------------|
| Decimal | `123` | Base 10 |
| Octal | `0777` | Base 8, starts with `0` |
| Hexadecimal | `0xFF` | Base 16, starts with `0x` or `0X` |

Suffixes: `u`/`U` (unsigned), `l`/`L` (long), `ul`/`UL` (unsigned long)

### Floating-Point Literals

```
123.456
123.456e10
123.456E-10
.5
5.
```

Suffixes: `f`/`F` (float), `l`/`L` (long double)

### Character Literals

```c
'a'      // Simple character
'\n'     // Escape sequence
'\x41'   // Hex escape (A)
'\101'   // Octal escape (A)
```

Escape sequences: `\n`, `\t`, `\r`, `\\`, `\'`, `\"`, `\0`, `\a`, `\b`, `\f`, `\v`

### String Literals

```c
"hello world"
"line1\nline2"
"path\\to\\file"
```

Adjacent string literals are concatenated by the preprocessor.

### Operators

| Category | Operators |
|----------|-----------|
| Arithmetic | `+` `-` `*` `/` `%` |
| Comparison | `==` `!=` `<` `>` `<=` `>=` |
| Logical | `&&` `||` `!` |
| Bitwise | `&` `|` `^` `~` `<<` `>>` |
| Assignment | `=` `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=` |
| Increment/Decrement | `++` `--` |
| Member Access | `.` `->` |
| Other | `?` `:` `,` `sizeof` |

### Punctuation

```
( ) [ ] { } ; , . -> ...
```

## Lexer API

### Data Structures

```c
/* Lexer state */
typedef struct mcc_lexer {
    mcc_context_t *ctx;
    const char *source;      /* Source buffer */
    size_t source_len;       /* Source length */
    size_t pos;              /* Current position */
    const char *filename;
    int line;
    int column;
    int current;             /* Current character */
    mcc_token_t *peek_token; /* Lookahead buffer */
    bool at_bol;             /* At beginning of line? */
    bool has_space;          /* Whitespace before token? */
} mcc_lexer_t;

/* Token */
typedef struct mcc_token {
    mcc_token_type_t type;
    const char *text;        /* Token text */
    mcc_location_t location;
    bool at_bol;             /* At beginning of line? */
    bool has_space;          /* Whitespace before? */
    union {
        struct { uint64_t value; bool is_unsigned; bool is_long; } int_val;
        struct { double value; bool is_float; bool is_long; } float_val;
        struct { int value; } char_val;
        struct { const char *value; size_t length; } string_val;
    } literal;
    struct mcc_token *next;  /* For token lists */
} mcc_token_t;
```

### Functions

```c
/* Create and destroy */
mcc_lexer_t *mcc_lexer_create(mcc_context_t *ctx);
void mcc_lexer_destroy(mcc_lexer_t *lex);

/* Initialize with source */
void mcc_lexer_init_string(mcc_lexer_t *lex, const char *source, const char *filename);
void mcc_lexer_init_file(mcc_lexer_t *lex, const char *filename);

/* Token retrieval */
mcc_token_t *mcc_lexer_next(mcc_lexer_t *lex);   /* Consume and return next token */
mcc_token_t *mcc_lexer_peek(mcc_lexer_t *lex);   /* Peek at next token */

/* Token matching */
bool mcc_lexer_match(mcc_lexer_t *lex, mcc_token_type_t type);
bool mcc_lexer_check(mcc_lexer_t *lex, mcc_token_type_t type);
mcc_token_t *mcc_lexer_expect(mcc_lexer_t *lex, mcc_token_type_t type, const char *msg);
```

## Token Type Enumeration

```c
typedef enum {
    /* Special */
    TOK_EOF,
    TOK_NEWLINE,
    TOK_IDENT,
    
    /* Literals */
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_CHAR_LIT,
    TOK_STRING_LIT,
    
    /* Keywords */
    TOK_AUTO, TOK_BREAK, TOK_CASE, TOK_CHAR, TOK_CONST,
    TOK_CONTINUE, TOK_DEFAULT, TOK_DO, TOK_DOUBLE, TOK_ELSE,
    TOK_ENUM, TOK_EXTERN, TOK_FLOAT, TOK_FOR, TOK_GOTO,
    TOK_IF, TOK_INT, TOK_LONG, TOK_REGISTER, TOK_RETURN,
    TOK_SHORT, TOK_SIGNED, TOK_SIZEOF, TOK_STATIC, TOK_STRUCT,
    TOK_SWITCH, TOK_TYPEDEF, TOK_UNION, TOK_UNSIGNED, TOK_VOID,
    TOK_VOLATILE, TOK_WHILE,
    
    /* Operators */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_AMP, TOK_PIPE, TOK_CARET, TOK_TILDE,
    TOK_LSHIFT, TOK_RSHIFT,
    TOK_EQ, TOK_NE, TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_AND, TOK_OR, TOK_NOT,
    TOK_ASSIGN, TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN, /* ... */
    TOK_INC, TOK_DEC,
    TOK_DOT, TOK_ARROW,
    TOK_QUESTION, TOK_COLON,
    
    /* Punctuation */
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_LBRACE, TOK_RBRACE,
    TOK_SEMICOLON, TOK_COMMA,
    TOK_ELLIPSIS,
    TOK_HASH,
    
    /* ... more token types */
} mcc_token_type_t;
```

## Implementation Details

### Keyword Recognition

Keywords are recognized using a static lookup table:

```c
static const struct {
    const char *name;
    mcc_token_type_t type;
} keywords[] = {
    {"auto",     TOK_AUTO},
    {"break",    TOK_BREAK},
    {"case",     TOK_CASE},
    /* ... */
};
```

After scanning an identifier, the lexer checks if it matches a keyword.

### Number Parsing

Numbers are parsed based on their prefix:
- `0x` or `0X`: Hexadecimal
- `0` followed by digits: Octal
- Otherwise: Decimal

Floating-point numbers are detected by the presence of `.` or `e`/`E`.

### Comment Handling

The lexer skips comments:
- Block comments: `/* ... */`
- Line comments are NOT supported (C99 feature)

### Whitespace Handling

Whitespace is skipped but tracked:
- `at_bol`: True if token is at beginning of line (for preprocessor)
- `has_space`: True if whitespace preceded the token

### Error Recovery

On invalid characters, the lexer:
1. Reports an error with location
2. Skips the invalid character
3. Continues lexing

## Usage Example

```c
mcc_context_t *ctx = mcc_context_create();
mcc_lexer_t *lex = mcc_lexer_create(ctx);

mcc_lexer_init_string(lex, "int x = 42;", "test.c");

mcc_token_t *tok;
while ((tok = mcc_lexer_next(lex))->type != TOK_EOF) {
    printf("Token: %s (%d) at line %d\n", 
           tok->text, tok->type, tok->location.line);
}

mcc_lexer_destroy(lex);
mcc_context_destroy(ctx);
```

## Preprocessor Integration

The lexer is used by the preprocessor, which:
1. Reads tokens from the lexer
2. Processes `#` directives
3. Expands macros
4. Outputs a new token stream for the parser

The `at_bol` flag is crucial for detecting preprocessor directives.
