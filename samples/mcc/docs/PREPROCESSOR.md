# MCC Preprocessor Documentation

This document describes the preprocessor component of MCC.

## Overview

The preprocessor handles all `#` directives before the source code is parsed. It performs:
- Macro definition and expansion
- Conditional compilation
- File inclusion
- Line control

The preprocessor is **C standard aware** and enables/disables features based on the selected standard (C89, C99, C11, C23, GNU extensions).

## Source File Organization

The preprocessor is organized into modular source files in `src/preprocessor/`:

| File | Description |
|------|-------------|
| `pp_internal.h` | Internal header with structures and function declarations |
| `preprocessor.c` | Main module - public API and preprocessing loop |
| `pp_macro.c` | Macro definition, lookup, and expansion |
| `pp_expr.c` | Preprocessor expression evaluation (#if expressions) |
| `pp_directive.c` | Directive processing (#if, #ifdef, #elif, #else, #endif, etc.) |
| `pp_include.c` | Include file processing and include stack management |

## Supported Directives

### Standard Directives (C89+)

| Directive | Description |
|-----------|-------------|
| `#define` | Define a macro |
| `#undef` | Undefine a macro |
| `#include` | Include a file |
| `#if` | Conditional compilation |
| `#ifdef` | If macro is defined |
| `#ifndef` | If macro is not defined |
| `#elif` | Else if |
| `#else` | Else branch |
| `#endif` | End conditional |
| `#error` | Generate error message |
| `#line` | Set line number |
| `#pragma` | Implementation-defined behavior |

### C99+ Directives

| Directive | Description | Standard |
|-----------|-------------|----------|
| `_Pragma()` | Pragma operator | C99 |
| Variadic macros | `__VA_ARGS__` | C99 |

### C23 Directives

| Directive | Description | Standard |
|-----------|-------------|----------|
| `#elifdef` | Else if defined | C23 |
| `#elifndef` | Else if not defined | C23 |
| `__VA_OPT__` | Optional variadic expansion | C23 |

### GNU Extensions

| Directive | Description |
|-----------|-------------|
| `#warning` | Generate warning message |
| `#include_next` | Include next file in search path |

## Macro Definition

### Object-like Macros

```c
#define NAME replacement-text

/* Examples */
#define VERSION 100
#define PI 3.14159
#define EMPTY
```

### Function-like Macros

```c
#define NAME(params) replacement-text

/* Examples */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define SQUARE(x) ((x) * (x))
#define DEBUG_PRINT(fmt, ...) printf(fmt, __VA_ARGS__)
```

**Important:** No space between macro name and `(` for function-like macros.

### Macro Expansion

Macros are expanded recursively:

```c
#define A 1
#define B A + 1
#define C B + 1

int x = C;  /* Expands to: int x = 1 + 1 + 1; */
```

### Predefined Macros

| Macro | Description |
|-------|-------------|
| `__FILE__` | Current filename (string) |
| `__LINE__` | Current line number (integer) |
| `__DATE__` | Compilation date (string) |
| `__TIME__` | Compilation time (string) |

## Conditional Compilation

### #ifdef / #ifndef

```c
#define DEBUG

#ifdef DEBUG
    /* Compiled when DEBUG is defined */
#endif

#ifndef RELEASE
    /* Compiled when RELEASE is NOT defined */
#endif
```

### #if / #elif / #else

```c
#define PLATFORM 1

#if PLATFORM == 0
    /* Windows */
#elif PLATFORM == 1
    /* Linux */
#elif PLATFORM == 2
    /* macOS */
#else
    /* Unknown */
#endif
```

### Conditional Expressions

The preprocessor supports these operators in `#if` expressions:

| Operator | Description |
|----------|-------------|
| `+` `-` `*` `/` `%` | Arithmetic |
| `==` `!=` `<` `>` `<=` `>=` | Comparison |
| `&&` `||` `!` | Logical |
| `&` `|` `^` `~` `<<` `>>` | Bitwise |
| `defined(NAME)` | Check if macro is defined |
| `(` `)` | Grouping |

```c
#define A 5
#define B 3

#if A + B > 7
    /* True: 5 + 3 = 8 > 7 */
#endif

#if defined(A) && !defined(C)
    /* True: A is defined, C is not */
#endif
```

## File Inclusion

### Local Files

```c
#include "myheader.h"
```

Search order:
1. Directory of the current file
2. Include paths specified with `-I`

### System Files

```c
#include <stdio.h>
```

Search order:
1. Include paths specified with `-I`
2. Standard system directories

### Include Guards

```c
#ifndef MYHEADER_H
#define MYHEADER_H

/* Header contents */

#endif /* MYHEADER_H */
```

## Preprocessor API

### Data Structures

```c
/* Macro parameter */
typedef struct mcc_macro_param {
    const char *name;
    struct mcc_macro_param *next;
} mcc_macro_param_t;

/* Macro definition */
typedef struct mcc_macro {
    const char *name;
    bool is_function_like;
    bool is_variadic;
    mcc_macro_param_t *params;
    int num_params;
    mcc_token_t *body;          /* Replacement tokens */
    mcc_location_t def_loc;
    struct mcc_macro *next;     /* Hash chain */
} mcc_macro_t;

/* Include file stack entry */
typedef struct mcc_include_file {
    const char *filename;
    const char *content;
    const char *pos;            /* Current position */
    int line;
    int column;
    struct mcc_include_file *next;
} mcc_include_file_t;

/* Conditional stack entry */
typedef struct mcc_cond_stack {
    bool condition;             /* Current branch active? */
    bool has_else;              /* #else seen? */
    bool any_true;              /* Any branch taken? */
    mcc_location_t location;
    struct mcc_cond_stack *next;
} mcc_cond_stack_t;

/* Preprocessor state */
typedef struct mcc_preprocessor {
    mcc_context_t *ctx;
    mcc_lexer_t *lexer;
    
    mcc_macro_t **macros;       /* Hash table */
    size_t macro_table_size;
    
    mcc_include_file_t *include_stack;
    int include_depth;
    
    mcc_cond_stack_t *cond_stack;
    bool skip_mode;
    
    const char **include_paths;
    size_t num_include_paths;
    
    mcc_token_t *output_head;
    mcc_token_t *output_tail;
    mcc_token_t *current;
    
    /* Macro expansion tracking */
    const char **expanding;
    size_t num_expanding;
    size_t cap_expanding;
} mcc_preprocessor_t;
```

### Functions

```c
/* Create and destroy */
mcc_preprocessor_t *mcc_preprocessor_create(mcc_context_t *ctx);
void mcc_preprocessor_destroy(mcc_preprocessor_t *pp);

/* Configuration */
void mcc_preprocessor_add_include_path(mcc_preprocessor_t *pp, const char *path);
void mcc_preprocessor_define(mcc_preprocessor_t *pp, const char *name, const char *value);
void mcc_preprocessor_undef(mcc_preprocessor_t *pp, const char *name);

/* Processing */
mcc_token_t *mcc_preprocessor_run(mcc_preprocessor_t *pp, const char *filename);
mcc_token_t *mcc_preprocessor_run_string(mcc_preprocessor_t *pp, 
                                          const char *source, const char *filename);

/* Token access */
mcc_token_t *mcc_preprocessor_next(mcc_preprocessor_t *pp);
mcc_token_t *mcc_preprocessor_peek(mcc_preprocessor_t *pp);

/* Query */
bool mcc_preprocessor_is_defined(mcc_preprocessor_t *pp, const char *name);
```

## Implementation Details

### Macro Table

Macros are stored in a hash table for O(1) lookup:

```c
static unsigned hash_string(const char *s)
{
    unsigned h = 0;
    while (*s) {
        h = h * 31 + (unsigned char)*s++;
    }
    return h;
}

static mcc_macro_t *pp_lookup_macro(mcc_preprocessor_t *pp, const char *name)
{
    unsigned h = hash_string(name) % pp->macro_table_size;
    for (mcc_macro_t *m = pp->macros[h]; m; m = m->next) {
        if (strcmp(m->name, name) == 0) {
            return m;
        }
    }
    return NULL;
}
```

### Conditional Compilation Stack

Nested conditionals are handled with a stack:

```c
static void pp_push_cond(mcc_preprocessor_t *pp, bool condition, mcc_location_t loc)
{
    mcc_cond_stack_t *cond = mcc_alloc(pp->ctx, sizeof(mcc_cond_stack_t));
    cond->condition = condition;
    cond->any_true = condition;
    cond->has_else = false;
    cond->location = loc;
    cond->next = pp->cond_stack;
    pp->cond_stack = cond;
    
    /* Update skip mode */
    pp->skip_mode = !condition;
    for (mcc_cond_stack_t *c = cond->next; c; c = c->next) {
        if (!c->condition) {
            pp->skip_mode = true;
            break;
        }
    }
}
```

### Include File Handling

When processing `#include`:

1. Find and open the file
2. Push current lexer state onto include stack
3. Initialize lexer with new file
4. Continue processing
5. On EOF, pop include stack and restore previous state

```c
static void pp_process_include(mcc_preprocessor_t *pp)
{
    /* Parse filename */
    /* ... */
    
    /* Check include depth */
    if (pp->include_depth >= MCC_MAX_INCLUDE_DEPTH) {
        mcc_error(pp->ctx, "Include depth limit exceeded");
        return;
    }
    
    /* Save current state */
    mcc_include_file_t *inc = mcc_alloc(pp->ctx, sizeof(mcc_include_file_t));
    inc->filename = pp->lexer->filename;
    inc->content = pp->lexer->source;
    inc->pos = pp->lexer->source + pp->lexer->pos;
    inc->line = pp->lexer->line;
    inc->column = pp->lexer->column;
    inc->next = pp->include_stack;
    pp->include_stack = inc;
    pp->include_depth++;
    
    /* Initialize lexer with new file */
    mcc_lexer_init_string(pp->lexer, content, path);
}
```

### Expression Evaluation

Preprocessor expressions are evaluated with a recursive descent evaluator:

```c
static int64_t pp_eval_expr(mcc_preprocessor_t *pp);
static int64_t pp_eval_primary(mcc_preprocessor_t *pp);
static int64_t pp_eval_unary(mcc_preprocessor_t *pp);
static int64_t pp_eval_multiplicative(mcc_preprocessor_t *pp);
static int64_t pp_eval_additive(mcc_preprocessor_t *pp);
/* ... etc for each precedence level */
```

## Usage Example

```c
mcc_context_t *ctx = mcc_context_create();
mcc_preprocessor_t *pp = mcc_preprocessor_create(ctx);

/* Add include paths */
mcc_preprocessor_add_include_path(pp, "includes");
mcc_preprocessor_add_include_path(pp, "/usr/include");

/* Define macros from command line */
mcc_preprocessor_define(pp, "DEBUG", "1");
mcc_preprocessor_define(pp, "VERSION", "100");

/* Process file */
mcc_token_t *tokens = mcc_preprocessor_run(pp, "input.c");

/* Iterate tokens */
for (mcc_token_t *tok = tokens; tok->type != TOK_EOF; tok = tok->next) {
    printf("%s ", tok->text);
}

mcc_preprocessor_destroy(pp);
mcc_context_destroy(ctx);
```

## Error Handling

The preprocessor reports errors for:
- Unterminated conditional directives
- `#elif` or `#else` without `#if`
- Duplicate `#else`
- Unmatched `#endif`
- Missing filename after `#include`
- File not found
- Macro redefinition (warning)
- Invalid preprocessor expression

## C Standard Feature Checks

The preprocessor checks C standard features before enabling certain functionality:

```c
/* In pp_internal.h - helper functions for feature checks */
static inline bool pp_has_variadic_macros(mcc_preprocessor_t *pp)
{
    return pp_has_feature(pp, MCC_FEAT_PP_VARIADIC);
}

static inline bool pp_has_line_comments(mcc_preprocessor_t *pp)
{
    return pp_has_feature(pp, MCC_FEAT_LINE_COMMENT);
}

static inline bool pp_has_elifdef(mcc_preprocessor_t *pp)
{
    return pp_has_feature(pp, MCC_FEAT_PP_ELIFDEF);
}
```

### Feature-Dependent Behavior

| Feature | C89 | C99 | C11 | C23 | GNU |
|---------|-----|-----|-----|-----|-----|
| `#` (stringification) | ✅ | ✅ | ✅ | ✅ | ✅ |
| `##` (token pasting) | ✅ | ✅ | ✅ | ✅ | ✅ |
| `__VA_ARGS__` | ❌ | ✅ | ✅ | ✅ | ✅ |
| `_Pragma()` | ❌ | ✅ | ✅ | ✅ | ✅ |
| `#elifdef` | ❌ | ❌ | ❌ | ✅ | ❌ |
| `#elifndef` | ❌ | ❌ | ❌ | ✅ | ❌ |
| `__VA_OPT__` | ❌ | ❌ | ❌ | ✅ | ❌ |
| `#warning` | ❌ | ❌ | ❌ | ❌ | ✅ |
| `#include_next` | ❌ | ❌ | ❌ | ❌ | ✅ |
| `//` comments | ❌ | ✅ | ✅ | ✅ | ✅ |

### Standard-Specific Predefined Macros

The preprocessor automatically defines macros based on the selected standard:

```c
/* C89/C90 */
__STDC__        /* 1 */

/* C99 */
__STDC__            /* 1 */
__STDC_VERSION__    /* 199901L */
__STDC_HOSTED__     /* 1 */

/* C11 */
__STDC__            /* 1 */
__STDC_VERSION__    /* 201112L */
__STDC_HOSTED__     /* 1 */

/* C23 */
__STDC__            /* 1 */
__STDC_VERSION__    /* 202311L */
__STDC_HOSTED__     /* 1 */

/* GNU modes add */
__GNUC__            /* 4 */
__GNUC_MINOR__      /* 0 */
```

## Stringification Operator (`#`)

The `#` operator converts a macro argument to a string literal (C89+):

```c
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define VERSION 100

const char *s1 = STRINGIFY(hello);      /* "hello" */
const char *s2 = TOSTRING(VERSION);     /* "100" */

/* With __VA_ARGS__ (C99+) */
#define DEBUG_VAR(var) printf(#var " = %d\n", var)
DEBUG_VAR(count);  /* printf("count" " = %d\n", count); */
```

### Stringification Rules

1. Leading and trailing whitespace is removed
2. Internal whitespace is preserved as single spaces
3. String literals and character constants are escaped
4. The result is always a valid string literal

## Variadic Macros (C99+)

### Basic `__VA_ARGS__`

```c
#define LOG(fmt, ...) printf(fmt, __VA_ARGS__)
#define DEBUG(...) fprintf(stderr, __VA_ARGS__)

LOG("Value: %d\n", 42);     /* printf("Value: %d\n", 42) */
DEBUG("Error: %s\n", msg);  /* fprintf(stderr, "Error: %s\n", msg) */
```

### Nested Variadic Macros

```c
#define INNER(level, fmt, ...) printf("[%s] " fmt "\n", level, __VA_ARGS__)
#define DEBUG(fmt, ...) INNER("DEBUG", fmt, __VA_ARGS__)
#define INFO(fmt, ...)  INNER("INFO", fmt, __VA_ARGS__)

DEBUG("x = %d", 42);  /* printf("[DEBUG] " "x = %d" "\n", "DEBUG", 42) */
```

### `__VA_OPT__` (C23)

The `__VA_OPT__` operator conditionally expands content only when variadic arguments are present:

```c
#define LOG(fmt, ...) printf(fmt __VA_OPT__(,) __VA_ARGS__)
#define CALL(f, ...) f(__VA_OPT__(__VA_ARGS__))

LOG("no args");           /* printf("no args") */
LOG("x = %d", 42);        /* printf("x = %d", 42) */

CALL(func);               /* func() */
CALL(func, 1, 2);         /* func(1, 2) */
```

**Note:** `__VA_OPT__` requires `-std=c23` or later.

## Token Pasting Operator (`##`)

The `##` operator concatenates two tokens into a single token (C89+):

```c
#define PASTE(a, b) a ## b
#define MAKE_VAR(n) var_ ## n
#define MAKE_FUNC(name) func_ ## name

int PASTE(my, var) = 42;    /* int myvar = 42; */
int MAKE_VAR(1) = 1;        /* int var_1 = 1; */
void MAKE_FUNC(test)(void); /* void func_test(void); */

/* Creating identifiers from numbers */
#define CONCAT_NUM(a, b) a ## b
int x = CONCAT_NUM(12, 34); /* int x = 1234; */
```

### Token Pasting Rules

1. The `##` operator cannot appear at the beginning or end of a macro body
2. Both operands are converted to their text representation before concatenation
3. The result is re-lexed to produce a valid token
4. If the result is not a valid token, a warning is issued
5. Whitespace around `##` is ignored

### Common Use Cases

```c
/* Generic data structure generation */
#define DECLARE_LIST(type) \
    typedef struct type ## _list { \
        type *data; \
        int size; \
    } type ## _list_t

DECLARE_LIST(int);   /* Creates int_list and int_list_t */

/* Function name generation */
#define TEST(name) void test_ ## name(void)
TEST(addition) { /* ... */ }  /* void test_addition(void) */
```

## Limitations

Current limitations of the MCC preprocessor:

1. **No `#pragma once`**: Use traditional include guards instead
2. **No computed includes**: `#include` with macro-expanded filename is not supported
