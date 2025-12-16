# MCC Parser Documentation

This document describes the parser component of MCC.

## Overview

The parser converts a stream of tokens into an Abstract Syntax Tree (AST). MCC uses a recursive descent parser, which is straightforward to implement and debug.

## File Organization

The parser is organized into modular files in `src/parser/`:

| File | Description |
|------|-------------|
| `parse_internal.h` | Internal header with structures, function declarations, and C standard feature checks |
| `parser.c` | Main module - public API, token operations, entry points |
| `parse_expr.c` | Expression parsing (primary, postfix, unary, binary, ternary, assignment) |
| `parse_stmt.c` | Statement parsing (if, while, for, switch, return, goto, labels) |
| `parse_type.c` | Type parsing (specifiers, qualifiers, struct/union/enum) |
| `parse_decl.c` | Declaration parsing (variables, functions, typedef, initializers) |

## Parsing Strategy

### Recursive Descent

Each grammar rule corresponds to a parsing function:

```c
/* In parse_decl.c */
mcc_ast_node_t *parse_declaration(mcc_parser_t *p);

/* In parse_stmt.c */
mcc_ast_node_t *parse_statement(mcc_parser_t *p);

/* In parse_expr.c */
mcc_ast_node_t *parse_expression(mcc_parser_t *p);

/* In parse_type.c */
mcc_type_t *parse_type_specifier(mcc_parser_t *p);
```

### Operator Precedence

Expressions are parsed using precedence climbing:

| Precedence | Operators | Associativity |
|------------|-----------|---------------|
| 15 | `,` | Left |
| 14 | `=` `+=` `-=` etc. | Right |
| 13 | `?:` | Right |
| 12 | `||` | Left |
| 11 | `&&` | Left |
| 10 | `|` | Left |
| 9 | `^` | Left |
| 8 | `&` | Left |
| 7 | `==` `!=` | Left |
| 6 | `<` `>` `<=` `>=` | Left |
| 5 | `<<` `>>` | Left |
| 4 | `+` `-` | Left |
| 3 | `*` `/` `%` | Left |
| 2 | Unary `!` `~` `-` `+` `*` `&` `++` `--` `sizeof` `(type)` | Right |
| 1 | Postfix `()` `[]` `.` `->` `++` `--` | Left |

## Parser API

### Data Structures

```c
/* Struct type registry */
typedef struct mcc_struct_entry {
    const char *tag;
    struct mcc_type *type;
    struct mcc_struct_entry *next;
} mcc_struct_entry_t;

/* Typedef registry */
typedef struct mcc_typedef_entry {
    const char *name;
    struct mcc_type *type;
    struct mcc_typedef_entry *next;
} mcc_typedef_entry_t;

/* Parser state */
typedef struct mcc_parser {
    mcc_context_t *ctx;
    mcc_preprocessor_t *pp;     /* Token source */
    mcc_token_t *current;       /* Current token */
    mcc_token_t *peek;          /* Lookahead token */
    struct mcc_symtab *symtab;  /* For typedef names */
    mcc_struct_entry_t *struct_types;  /* Struct registry */
    mcc_typedef_entry_t *typedefs;     /* Typedef registry */
    bool panic_mode;            /* Error recovery */
    int sync_depth;
} mcc_parser_t;
```

### Functions

```c
/* Create and destroy */
mcc_parser_t *mcc_parser_create(mcc_context_t *ctx, mcc_preprocessor_t *pp);
void mcc_parser_destroy(mcc_parser_t *parser);

/* Main entry point */
mcc_ast_node_t *mcc_parser_parse(mcc_parser_t *parser);

/* Token operations */
mcc_token_t *mcc_parser_advance(mcc_parser_t *parser);
bool mcc_parser_check(mcc_parser_t *parser, mcc_token_type_t type);
bool mcc_parser_match(mcc_parser_t *parser, mcc_token_type_t type);
mcc_token_t *mcc_parser_expect(mcc_parser_t *parser, mcc_token_type_t type, const char *msg);
```

## AST Node Types

### Declaration Nodes

```c
/* Translation unit - root of AST */
AST_TRANS_UNIT {
    mcc_ast_node_t **decls;
    size_t num_decls;
}

/* Function declaration/definition */
AST_FUNC_DECL {
    const char *name;
    mcc_type_t *return_type;
    mcc_ast_node_t **params;    /* AST_PARAM_DECL nodes */
    size_t num_params;
    mcc_ast_node_t *body;       /* AST_COMPOUND_STMT or NULL */
    mcc_storage_class_t storage;
}

/* Typedef declaration */
AST_TYPEDEF_DECL {
    const char *name;
    mcc_type_t *type;
}

/* Variable declaration */
AST_VAR_DECL {
    const char *name;
    mcc_type_t *var_type;
    mcc_ast_node_t *init;       /* Initializer or NULL */
    mcc_storage_class_t storage;
}

/* Parameter declaration */
AST_PARAM_DECL {
    const char *name;           /* Can be NULL for prototypes */
    mcc_type_t *param_type;
}
```

### Statement Nodes

```c
/* Compound statement (block) */
AST_COMPOUND_STMT {
    mcc_ast_node_t **stmts;
    size_t num_stmts;
}

/* Expression statement */
AST_EXPR_STMT {
    mcc_ast_node_t *expr;
}

/* If statement */
AST_IF_STMT {
    mcc_ast_node_t *cond;
    mcc_ast_node_t *then_stmt;
    mcc_ast_node_t *else_stmt;  /* NULL if no else */
}

/* While statement */
AST_WHILE_STMT {
    mcc_ast_node_t *cond;
    mcc_ast_node_t *body;
}

/* For statement */
AST_FOR_STMT {
    mcc_ast_node_t *init;       /* Expression or declaration */
    mcc_ast_node_t *cond;
    mcc_ast_node_t *incr;
    mcc_ast_node_t *body;
}

/* Return statement */
AST_RETURN_STMT {
    mcc_ast_node_t *value;      /* NULL for void return */
}

/* Switch statement */
AST_SWITCH_STMT {
    mcc_ast_node_t *expr;
    mcc_ast_node_t *body;
}

/* Case label */
AST_CASE_STMT {
    mcc_ast_node_t *value;      /* Constant expression */
    mcc_ast_node_t *stmt;
}

/* Goto and label */
AST_GOTO_STMT { const char *label; }
AST_LABEL_STMT { const char *label; mcc_ast_node_t *stmt; }
```

### Expression Nodes

```c
/* Literals */
AST_INT_LIT { uint64_t value; bool is_unsigned; bool is_long; }
AST_FLOAT_LIT { double value; bool is_float; }
AST_CHAR_LIT { int value; }
AST_STRING_LIT { const char *value; size_t length; }

/* Identifier */
AST_IDENT_EXPR {
    const char *name;
    mcc_symbol_t *symbol;       /* Resolved by sema */
}

/* Binary expression */
AST_BINARY_EXPR {
    mcc_binop_t op;
    mcc_ast_node_t *lhs;
    mcc_ast_node_t *rhs;
}

/* Unary expression */
AST_UNARY_EXPR {
    mcc_unop_t op;
    mcc_ast_node_t *operand;
}

/* Ternary expression */
AST_TERNARY_EXPR {
    mcc_ast_node_t *cond;
    mcc_ast_node_t *then_expr;
    mcc_ast_node_t *else_expr;
}

/* Function call */
AST_CALL_EXPR {
    mcc_ast_node_t *func;
    mcc_ast_node_t **args;
    size_t num_args;
}

/* Array subscript */
AST_SUBSCRIPT_EXPR {
    mcc_ast_node_t *array;
    mcc_ast_node_t *index;
}

/* Member access (. and ->) */
AST_MEMBER_EXPR {
    mcc_ast_node_t *object;
    const char *member;
    bool is_arrow;              /* true for ->, false for . */
}

/* Type cast */
AST_CAST_EXPR {
    mcc_type_t *target_type;
    mcc_ast_node_t *expr;
}

/* Sizeof */
AST_SIZEOF_EXPR {
    mcc_type_t *type_arg;       /* sizeof(type) */
    mcc_ast_node_t *expr_arg;   /* sizeof expr */
}
```

## Binary Operators

```c
typedef enum {
    /* Arithmetic */
    BINOP_ADD, BINOP_SUB, BINOP_MUL, BINOP_DIV, BINOP_MOD,
    
    /* Bitwise */
    BINOP_AND, BINOP_OR, BINOP_XOR, BINOP_LSHIFT, BINOP_RSHIFT,
    
    /* Comparison */
    BINOP_EQ, BINOP_NE, BINOP_LT, BINOP_GT, BINOP_LE, BINOP_GE,
    
    /* Logical */
    BINOP_LOG_AND, BINOP_LOG_OR,
    
    /* Assignment */
    BINOP_ASSIGN,
    BINOP_ADD_ASSIGN, BINOP_SUB_ASSIGN, BINOP_MUL_ASSIGN,
    BINOP_DIV_ASSIGN, BINOP_MOD_ASSIGN,
    BINOP_AND_ASSIGN, BINOP_OR_ASSIGN, BINOP_XOR_ASSIGN,
    BINOP_LSHIFT_ASSIGN, BINOP_RSHIFT_ASSIGN,
    
    /* Other */
    BINOP_COMMA
} mcc_binop_t;
```

## Unary Operators

```c
typedef enum {
    UNOP_NEG,       /* -x */
    UNOP_POS,       /* +x */
    UNOP_NOT,       /* !x */
    UNOP_BIT_NOT,   /* ~x */
    UNOP_DEREF,     /* *x */
    UNOP_ADDR,      /* &x */
    UNOP_PRE_INC,   /* ++x */
    UNOP_PRE_DEC,   /* --x */
    UNOP_POST_INC,  /* x++ */
    UNOP_POST_DEC   /* x-- */
} mcc_unop_t;
```

## Type Parsing (`parse_type.c`)

Types are parsed by `parse_type_specifier()`:

```c
mcc_type_t *parse_type_specifier(mcc_parser_t *p)
{
    /* Parse qualifiers: const, volatile, restrict (C99) */
    /* Parse type specifiers: int, char, struct, _Bool (C99), etc. */
    /* Parse pointers: * */
    /* Return complete type */
}
```

### C Standard Feature Checks

The type parser checks for C standard features:

```c
/* C99: restrict qualifier */
if (!parse_has_restrict(p)) {
    mcc_warning_at(p->ctx, loc, "'restrict' is a C99 extension");
}

/* C99: _Bool type */
if (!parse_has_bool(p)) {
    mcc_warning_at(p->ctx, loc, "'_Bool' is a C99 extension");
}

/* C99: long long */
if (!parse_has_long_long(p)) {
    mcc_warning_at(p->ctx, loc, "'long long' is a C99 extension");
}
```

### Struct Parsing

Structs are handled specially to support forward references:

1. When a struct definition is parsed, it's registered in `parser->struct_types`
2. When a struct reference is parsed, it looks up the registered type
3. Forward declarations create incomplete types that are completed later

```c
/* Definition: struct point { int x; int y; }; */
/* Reference: struct point *p; */
```

### C11: Anonymous Struct/Union Members

```c
struct outer {
    int a;
    struct { int x, y; };  /* C11 anonymous struct member */
};
```

## Error Recovery

The parser uses panic mode for error recovery:

1. On error, set `panic_mode = true`
2. Skip tokens until a synchronization point
3. Synchronization points: `;`, `}`, keywords like `if`, `while`, etc.
4. Resume parsing

```c
void mcc_parser_synchronize(mcc_parser_t *p)
{
    p->panic_mode = false;
    
    while (p->peek->type != TOK_EOF) {
        if (p->current->type == TOK_SEMICOLON) return;
        
        switch (p->peek->type) {
            case TOK_IF:
            case TOK_WHILE:
            case TOK_FOR:
            case TOK_RETURN:
                return;
            default:
                mcc_parser_advance(p);
        }
    }
}
```

## Usage Example

```c
mcc_context_t *ctx = mcc_context_create();
mcc_preprocessor_t *pp = mcc_preprocessor_create(ctx);
mcc_preprocessor_run(pp, "test.c");

mcc_parser_t *parser = mcc_parser_create(ctx, pp);
mcc_ast_node_t *ast = mcc_parser_parse(parser);

if (ast && ctx->error_count == 0) {
    /* AST is valid, proceed to semantic analysis */
}

mcc_parser_destroy(parser);
mcc_preprocessor_destroy(pp);
mcc_context_destroy(ctx);
```

## C Standard Feature Checks

The parser uses feature checks from `parse_internal.h` to validate C standard compliance:

### Feature Check Functions

```c
/* C99 features */
parse_has_mixed_decl(p)      /* Declarations mixed with statements */
parse_has_for_decl(p)        /* Declarations in for-loop init */
parse_has_vla(p)             /* Variable Length Arrays */
parse_has_designated_init(p) /* Designated initializers */
parse_has_compound_lit(p)    /* Compound literals (type){ ... } */
parse_has_long_long(p)       /* long long type */
parse_has_restrict(p)        /* restrict qualifier */
parse_has_inline(p)          /* inline function specifier */
parse_has_bool(p)            /* _Bool type */

/* C11 features */
parse_has_alignof(p)         /* _Alignof operator */
parse_has_static_assert(p)   /* _Static_assert */
parse_has_generic(p)         /* _Generic selection */
parse_has_noreturn(p)        /* _Noreturn specifier */
parse_has_atomic(p)          /* _Atomic qualifier */
parse_has_anonymous_struct(p)/* Anonymous struct/union members */

/* C23 features */
parse_has_nullptr(p)         /* nullptr constant */
parse_has_true_false(p)      /* true/false as keywords */
parse_has_bool_keyword(p)    /* bool as keyword (not _Bool) */
parse_has_constexpr(p)       /* constexpr specifier */
parse_has_typeof(p)          /* typeof operator */

/* GNU extensions */
parse_has_stmt_expr(p)       /* Statement expressions ({ ... }) */
parse_has_label_addr(p)      /* Labels as values (&&label) */
parse_has_case_range(p)      /* Case ranges (case 1 ... 5:) */
```

### Usage Example

```c
/* In parse_stmt.c - for loop with declaration */
if (parse_is_declaration_start(p)) {
    if (!parse_has_for_decl(p)) {
        mcc_warning_at(p->ctx, p->peek->location,
            "declaration in for loop initializer is a C99 extension");
    }
    init_decl = parse_declaration(p);
}
```

## New AST Node Types (C99+)

| Node Type | Standard | Description |
|-----------|----------|-------------|
| `AST_COMPOUND_LIT` | C99 | Compound literal `(type){ init }` |
| `AST_DESIGNATED_INIT` | C99 | Designated initializer `.field = value` |
| `AST_FIELD_DESIGNATOR` | C99 | Field designator in initializer |
| `AST_INDEX_DESIGNATOR` | C99 | Index designator in initializer |
| `AST_ALIGNOF_EXPR` | C11 | `_Alignof(type)` expression |
| `AST_STATIC_ASSERT` | C11 | `_Static_assert(expr, msg)` |
| `AST_GENERIC_EXPR` | C11 | `_Generic(expr, ...)` selection |
| `AST_NULL_PTR` | C23 | `nullptr` constant |
| `AST_STMT_EXPR` | GNU | Statement expression `({ ... })` |
| `AST_LABEL_ADDR` | GNU | Label address `&&label` |
| `AST_GOTO_EXPR` | GNU | Computed goto `goto *expr` |

## Grammar Summary

```
translation_unit = { declaration }

declaration = function_decl | var_decl | struct_decl | typedef_decl
            | static_assert_decl  /* C11 */

function_decl = type_spec IDENT '(' params ')' ( compound_stmt | ';' )

var_decl = type_spec IDENT [ '=' initializer ] ';'

initializer = assignment_expr | '{' initializer_list '}'
            | designator_list '=' initializer  /* C99 */

statement = compound_stmt | if_stmt | while_stmt | for_stmt | 
            return_stmt | expr_stmt | ...

compound_stmt = '{' { statement | declaration } '}'  /* C99: mixed decls */

for_stmt = 'for' '(' ( expr | declaration ) ';' expr ';' expr ')' stmt  /* C99: decl */

expression = assignment_expr { ',' assignment_expr }

assignment_expr = conditional_expr [ assign_op assignment_expr ]

conditional_expr = logical_or_expr [ '?' expr ':' conditional_expr ]

/* ... and so on for each precedence level */

primary_expr = IDENT | INT_LIT | FLOAT_LIT | STRING_LIT | '(' expr ')'
             | '(' type ')' '{' init_list '}'  /* C99: compound literal */
             | 'nullptr'                        /* C23 */
             | 'true' | 'false'                 /* C23 */
```
