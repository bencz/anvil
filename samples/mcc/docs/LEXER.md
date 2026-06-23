# MCC Lexer Documentation

This document describes the lexer (tokenizer) component of MCC.

## Overview

The lexer converts source code text into a stream of tokens. Each token represents a meaningful unit of the language: keywords, identifiers, literals, operators, and punctuation.

The lexer is **C standard aware** and enables/disables features based on the selected standard (C89, C99, C11, C23, GNU extensions).

## Source File Organization

The lexer is organized into modular source files in `src/lexer/`:

| File | Description |
|------|-------------|
| `lex_internal.h` | Internal header with structures and function declarations |
| `lexer.c` | Main module - public API and token scanning loop |
| `lex_token.c` | Token creation, names, and utility functions |
| `lex_char.c` | Character processing (advance, peek, whitespace) |
| `lex_comment.c` | Comment processing with C standard checks |
| `lex_string.c` | String and character literal processing |
| `lex_number.c` | Number literal processing with C99/C23 features |
| `lex_ident.c` | Identifier and keyword processing |
| `lex_operator.c` | Operator and punctuation processing |

## Token Types

### C89 Keywords (32 keywords)

```c
auto     break    case     char     const    continue
default  do       double   else     enum     extern
float    for      goto     if       int      long
register return   short    signed   sizeof   static
struct   switch   typedef  union    unsigned void
volatile while
```

### C99 Keywords (5 keywords)

| Keyword | Description | Token |
|---------|-------------|-------|
| `inline` | Inline function specifier | `TOK_INLINE` |
| `restrict` | Pointer qualifier | `TOK_RESTRICT` |
| `_Bool` | Boolean type | `TOK__BOOL` |
| `_Complex` | Complex number type | `TOK__COMPLEX` |
| `_Imaginary` | Imaginary number type | `TOK__IMAGINARY` |

### C11 Keywords (7 keywords)

| Keyword | Description | Token |
|---------|-------------|-------|
| `_Alignas` | Alignment specifier | `TOK__ALIGNAS` |
| `_Alignof` | Alignment query | `TOK__ALIGNOF` |
| `_Atomic` | Atomic type qualifier | `TOK__ATOMIC` |
| `_Generic` | Generic selection | `TOK__GENERIC` |
| `_Noreturn` | No-return function | `TOK__NORETURN` |
| `_Static_assert` | Static assertion | `TOK__STATIC_ASSERT` |
| `_Thread_local` | Thread-local storage | `TOK__THREAD_LOCAL` |

### C23 Keywords (12 keywords)

| Keyword | Description | Token |
|---------|-------------|-------|
| `true` | Boolean true | `TOK_TRUE` |
| `false` | Boolean false | `TOK_FALSE` |
| `nullptr` | Null pointer constant | `TOK_NULLPTR` |
| `constexpr` | Constant expression | `TOK_CONSTEXPR` |
| `typeof` | Type query | `TOK_TYPEOF` |
| `typeof_unqual` | Unqualified type query | `TOK_TYPEOF_UNQUAL` |
| `_BitInt` | Bit-precise integer type | `TOK__BITINT` |
| `alignas` | Alignment specifier (C23 spelling) | `TOK_ALIGNAS` |
| `alignof` | Alignment query (C23 spelling) | `TOK_ALIGNOF` |
| `bool` | Boolean type (C23 spelling) | `TOK_BOOL` |
| `static_assert` | Static assertion (C23 spelling) | `TOK_STATIC_ASSERT` |
| `thread_local` | Thread-local storage (C23 spelling) | `TOK_THREAD_LOCAL` |

### GNU Extension Keywords

Available under the GNU standards (`gnu89`/`gnu99`/`gnu11`):

| Keyword(s) | Description | Token |
|------------|-------------|-------|
| `__attribute__`, `__attribute` | GNU attribute syntax | `TOK_ATTRIBUTE` |
| `__typeof__`, `__typeof` | GNU `typeof` | `TOK_TYPEOF` |
| `asm`, `__asm__`, `__asm` | Inline assembly | `TOK_ASM` |

### Identifiers

Identifiers start with a letter or underscore, followed by letters, digits, or underscores:

```
[a-zA-Z_][a-zA-Z0-9_]*
```

### Integer Literals

| Format | Example | Description | Standard |
|--------|---------|-------------|----------|
| Decimal | `123` | Base 10 | C89 |
| Octal | `0777` | Base 8, starts with `0` | C89 |
| Hexadecimal | `0xFF` | Base 16, starts with `0x` or `0X` | C89 |
| Binary | `0b1010` | Base 2, starts with `0b` or `0B` | C23 |

**Suffixes:**
- `u`/`U` (unsigned) - C89
- `l`/`L` (long) - C89
- `ul`/`UL` (unsigned long) - C89
- `ll`/`LL` (long long) - C99
- `ull`/`ULL` (unsigned long long) - C99

### Floating-Point Literals

```c
123.456       /* Decimal float */
123.456e10    /* With exponent */
123.456E-10   /* Negative exponent */
.5            /* No integer part */
5.            /* No fractional part */
0x1.0p10      /* Hexadecimal float (C99) */
```

**Suffixes:**
- `f`/`F` (float) - C89
- `l`/`L` (long double) - C89

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
    mcc_location_t location;

    const char *text;        /* Token text (identifiers, literals) */
    size_t text_len;
    const char *raw_text;    /* Raw source slice incl. quotes (for pp output) */

    union {
        struct { uint64_t value; mcc_int_suffix_t suffix; } int_val;
        struct { double value;   mcc_float_suffix_t suffix; } float_val;
        struct { int value; } char_val;                       /* character code */
        struct { const char *value; size_t length; } string_val;
    } literal;

    bool at_bol;             /* At beginning of line? (for preprocessor) */
    bool has_space;          /* Whitespace before this token? */
    struct mcc_token *next;  /* Next token in list (for preprocessor) */
} mcc_token_t;
```

Integer and float literals record their suffix using the `mcc_int_suffix_t`
(`INT_SUFFIX_NONE`/`U`/`L`/`UL`/`LL`/`ULL`) and `mcc_float_suffix_t`
(`FLOAT_SUFFIX_NONE`/`F`/`L`) enums rather than individual boolean flags.

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

/* Current location */
mcc_location_t mcc_lexer_location(mcc_lexer_t *lex);

/* Token allocation (see "Token Allocation and Lifetime" below) */
mcc_token_t *mcc_token_create(mcc_context_t *ctx);
mcc_token_t *mcc_token_copy(mcc_context_t *ctx, mcc_token_t *tok);
void mcc_token_list_free(mcc_token_t *list);
```

### Token Allocation and Lifetime

Tokens are allocated from the compiler context's **chunked, non-relocating arena**
(via `mcc_alloc`), not with individual `malloc`/`free` calls. As a result:

- Token pointers remain valid for the entire compilation; nothing moves or is
  individually freed.
- `lex_make_token()` and `mcc_token_create()` simply bump-allocate a token in the arena.
- `mcc_token_copy()` arena-duplicates the token and its `text`/`raw_text` strings
  (with `mcc_strdup`) and clears `next`.
- `mcc_token_list_free()` is effectively a **no-op** — the arena owns the memory and
  reclaims it all at once when the context is destroyed.

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

    /* C99/C11/C23 keywords */
    TOK_INLINE, TOK_RESTRICT, TOK__BOOL, TOK__COMPLEX, TOK__IMAGINARY,
    TOK__ALIGNAS, TOK__ALIGNOF, TOK__ATOMIC, TOK__GENERIC,
    TOK__NORETURN, TOK__STATIC_ASSERT, TOK__THREAD_LOCAL,
    TOK_TRUE, TOK_FALSE, TOK_NULLPTR, TOK_CONSTEXPR,
    TOK_TYPEOF, TOK_TYPEOF_UNQUAL, TOK__BITINT,
    TOK_ALIGNAS, TOK_ALIGNOF, TOK_BOOL, TOK_STATIC_ASSERT, TOK_THREAD_LOCAL,

    /* GNU extensions */
    TOK_ATTRIBUTE, TOK_ASM,
    
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
    TOK_HASH, TOK_HASH_HASH,    /* # and ## (preprocessor) */
    TOK_ELLIPSIS,              /* ... */
    TOK_NEWLINE,              /* used by the preprocessor */
    TOK_LBRACKET2, TOK_RBRACKET2,  /* [[ and ]] (C23 attributes) */

    TOK_COUNT
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

The lexer skips comments with C standard awareness:

| Comment Type | Syntax | Standard |
|--------------|--------|----------|
| Block comments | `/* ... */` | C89 |
| Line comments | `// ...` | C99+ |

**C Standard Behavior:**
- In C89 mode: `//` comments generate a warning but are still processed
- In C99+ or GNU modes: `//` comments are processed silently

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

## C Standard Feature Checks

The lexer checks C standard features before enabling certain functionality:

```c
/* In lex_internal.h - helper functions for feature checks */
static inline bool lex_has_line_comments(mcc_lexer_t *lex)
{
    return lex_has_feature(lex, MCC_FEAT_LINE_COMMENT);
}

static inline bool lex_has_long_long(mcc_lexer_t *lex)
{
    return lex_has_feature(lex, MCC_FEAT_LONG_LONG);
}

static inline bool lex_has_hex_floats(mcc_lexer_t *lex)
{
    return lex_has_feature(lex, MCC_FEAT_HEX_FLOAT);
}
```

### Feature-Dependent Behavior

| Feature | C89 | C99 | C11 | C23 | GNU |
|---------|-----|-----|-----|-----|-----|
| `//` comments | ⚠️ | ✅ | ✅ | ✅ | ✅ |
| `long long` suffix | ⚠️ | ✅ | ✅ | ✅ | ✅ |
| Hex floats (`0x1.0p0`) | ⚠️ | ✅ | ✅ | ✅ | ✅ |
| Binary literals (`0b`) | ⚠️ | ⚠️ | ⚠️ | ✅ | ⚠️ |
| Universal char (`\u`) | ❌ | ✅ | ✅ | ✅ | ✅ |
| Digraphs (`<%`, `%>`) | ❌ | ✅ | ✅ | ✅ | ✅ |
| `inline` keyword | ❌ | ✅ | ✅ | ✅ | ✅ |
| `restrict` keyword | ❌ | ✅ | ✅ | ✅ | ✅ |
| `_Bool` keyword | ❌ | ✅ | ✅ | ✅ | ✅ |
| `_Alignas` keyword | ❌ | ❌ | ✅ | ✅ | ❌ |
| `_Atomic` keyword | ❌ | ❌ | ✅ | ✅ | ❌ |
| `true`/`false` keywords | ❌ | ❌ | ❌ | ✅ | ❌ |
| `nullptr` keyword | ❌ | ❌ | ❌ | ✅ | ❌ |
| `bool` keyword | ❌ | ❌ | ❌ | ✅ | ❌ |

Legend: ✅ = supported, ⚠️ = warning + supported, ❌ = not available

### Keyword Table with Features

Keywords are stored with their required C standard feature:

```c
typedef struct {
    const char *name;
    mcc_token_type_t type;
    mcc_feature_id_t required_feature;  /* MCC_FEAT_COUNT = always available */
} lex_keyword_entry_t;

static const lex_keyword_entry_t keywords[] = {
    /* C89 keywords (always available) */
    {"auto",     TOK_AUTO,     MCC_FEAT_COUNT},
    {"int",      TOK_INT,      MCC_FEAT_COUNT},
    /* ... */
    
    /* C99 keywords */
    {"inline",   TOK_INLINE,   MCC_FEAT_INLINE},
    {"restrict", TOK_RESTRICT, MCC_FEAT_RESTRICT},
    {"_Bool",    TOK__BOOL,    MCC_FEAT_BOOL},
    /* ... */
    
    /* C11 keywords */
    {"_Alignas", TOK__ALIGNAS, MCC_FEAT_ALIGNAS},
    {"_Atomic",  TOK__ATOMIC,  MCC_FEAT_ATOMIC},
    /* ... */
    
    /* C23 keywords */
    {"true",     TOK_TRUE,     MCC_FEAT_TRUE_FALSE},
    {"false",    TOK_FALSE,    MCC_FEAT_TRUE_FALSE},
    {"nullptr",  TOK_NULLPTR,  MCC_FEAT_NULLPTR},
    /* ... */
};
```

When a keyword is not available in the current C standard, it is treated as an identifier.
