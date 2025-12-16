# MCC Architecture

This document describes the internal architecture of the MCC (Micro C Compiler) for developers who want to understand or extend the compiler.

## Overview

MCC is a complete C89 compiler frontend that generates code through the ANVIL intermediate representation. The compiler follows a traditional multi-pass architecture:

```
Source Code → Preprocessor → Lexer → Parser → Semantic Analysis → Code Generation → Assembly
```

### Multi-File Compilation

MCC supports compiling multiple source files into a single output. The compilation flow for multiple files:

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│   file1.c   │     │   file2.c   │     │   file3.c   │
└──────┬──────┘     └──────┬──────┘     └──────┬──────┘
       │                   │                   │
       ▼                   ▼                   ▼
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│ Preprocess  │     │ Preprocess  │     │ Preprocess  │
│   + Parse   │     │   + Parse   │     │   + Parse   │
└──────┬──────┘     └──────┬──────┘     └──────┬──────┘
       │                   │                   │
       ▼                   ▼                   ▼
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│    AST 1    │     │    AST 2    │     │    AST 3    │
└──────┬──────┘     └──────┬──────┘     └──────┬──────┘
       │                   │                   │
       └───────────────────┼───────────────────┘
                           │
                           ▼
                 ┌─────────────────┐
                 │  Shared Sema    │  (common symbol table)
                 └────────┬────────┘
                          │
                          ▼
                 ┌─────────────────┐
                 │ Shared Codegen  │  (single ANVIL module)
                 └────────┬────────┘
                          │
                          ▼
                 ┌─────────────────┐
                 │  Assembly Out   │
                 └─────────────────┘
```

Key aspects:
- Each file is preprocessed and parsed independently
- All ASTs share a common semantic analyzer (symbol table)
- All code is generated into a single ANVIL module
- Functions defined in one file can be called from another

## Compilation Pipeline

### 1. Preprocessor (`src/preprocessor/`)

The preprocessor is organized into modular files:

| File | Description |
|------|-------------|
| `pp_internal.h` | Internal header with structures and function declarations |
| `preprocessor.c` | Main module - public API and preprocessing loop |
| `pp_macro.c` | Macro definition, lookup, and expansion |
| `pp_expr.c` | Preprocessor expression evaluation |
| `pp_directive.c` | Directive processing (#if, #ifdef, etc.) |
| `pp_include.c` | Include file processing and stack management |

The preprocessor handles all `#` directives before parsing:

- **Macro expansion**: Object-like (`#define FOO 1`) and function-like (`#define MAX(a,b) ...`)
- **Conditional compilation**: `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`
- **File inclusion**: `#include <file>` and `#include "file"`
- **Other directives**: `#undef`, `#error`, `#warning`, `#line`, `#pragma`
- **C Standard aware**: Features like `#elifdef` (C23), `#warning` (GNU) are checked

**Key data structures:**
- `mcc_preprocessor_t`: Main preprocessor state
- `mcc_macro_t`: Macro definition (name, parameters, body tokens)
- `mcc_cond_stack_t`: Stack for nested conditional compilation
- `mcc_include_file_t`: Stack for nested includes

### 2. Lexer (`src/lexer/`)

The lexer is organized into modular files:

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

The lexer converts source text into tokens:

- **Keywords**: C89 (32), C99 (5), C11 (7), C23 (12) - standard-aware
- **Identifiers**: Variable and function names
- **Literals**: Integer, floating-point, character, string (with C99/C23 features)
- **Operators**: `+`, `-`, `*`, `/`, `==`, `!=`, `->`, etc.
- **Punctuation**: `{`, `}`, `(`, `)`, `;`, `,`, etc.
- **C Standard aware**: `//` comments, binary literals, hex floats, etc.

**Key data structures:**
- `mcc_lexer_t`: Lexer state (source, position, line/column)
- `mcc_token_t`: Token with type, text, location, literal value
- `lex_keyword_entry_t`: Keyword with required C standard feature

### 3. Parser (`src/parser/`)

The parser is organized into modular files:

| File | Description |
|------|-------------|
| `parse_internal.h` | Internal header with structures, function declarations, and C standard feature checks |
| `parser.c` | Main module - public API, token operations, entry points |
| `parse_expr.c` | Expression parsing (primary, postfix, unary, binary, ternary, assignment) |
| `parse_stmt.c` | Statement parsing (if, while, for, switch, return, goto, labels) |
| `parse_type.c` | Type parsing (specifiers, qualifiers, struct/union/enum) |
| `parse_decl.c` | Declaration parsing (variables, functions, typedef, initializers) |

The parser builds an Abstract Syntax Tree (AST) using recursive descent:

- **Declarations**: Variables, functions, structs, unions, enums, typedefs
- **Statements**: if, while, for, do-while, switch, return, goto, labels
- **Expressions**: All C operators with correct precedence
- **Typedef handling**: Registers typedef names and recognizes them as types
- **C Standard aware**: Features like `for`-declarations (C99), compound literals (C99), `_Static_assert` (C11) are checked

**Key data structures:**
- `mcc_parser_t`: Parser state
- `mcc_ast_node_t`: AST node (50+ node types)
- `mcc_struct_entry_t`: Struct type registry for forward references
- `mcc_typedef_entry_t`: Typedef registry for type aliases

### 4. Semantic Analysis (`sema.c`)

Semantic analysis performs type checking and symbol resolution:

- **Type checking**: Ensure operands have compatible types
- **Symbol resolution**: Link identifiers to their declarations
- **Scope management**: Handle nested scopes correctly
- **Implicit conversions**: Integer promotions, usual arithmetic conversions

**Key data structures:**
- `mcc_sema_t`: Semantic analyzer state
- `mcc_symtab_t`: Symbol table (hash table with scope chain)
- `mcc_symbol_t`: Symbol entry (name, type, kind, scope)

### 5. Code Generation (`codegen.c`)

The code generator translates AST to ANVIL IR:

- **Function generation**: Prologue, body, epilogue
- **Expression evaluation**: Recursive traversal
- **Control flow**: Branches, loops, labels
- **Memory operations**: Load, store, alloca, GEP

**Key data structures:**
- `mcc_codegen_t`: Code generator state
- `anvil_ctx_t`: ANVIL context
- `anvil_module_t`: ANVIL module
- `anvil_func_t`: ANVIL function
- `anvil_value_t`: ANVIL value (SSA form)

## Memory Management

MCC uses arena allocation for most data structures:

```c
typedef struct mcc_arena {
    char *data;
    size_t size;
    size_t used;
    struct mcc_arena *next;
} mcc_arena_t;
```

Benefits:
- Fast allocation (bump pointer)
- No individual frees needed
- Entire arena freed at once at end of compilation

## Type System

The type system represents all C89 types:

```c
typedef enum {
    TYPE_VOID, TYPE_CHAR, TYPE_SHORT, TYPE_INT, TYPE_LONG,
    TYPE_FLOAT, TYPE_DOUBLE,
    TYPE_POINTER, TYPE_ARRAY, TYPE_FUNCTION,
    TYPE_STRUCT, TYPE_UNION, TYPE_ENUM
} mcc_type_kind_t;

typedef struct mcc_type {
    mcc_type_kind_t kind;
    int qualifiers;      /* QUAL_CONST, QUAL_VOLATILE */
    bool is_unsigned;
    union {
        struct { mcc_type_t *pointee; } pointer;
        struct { mcc_type_t *element; size_t size; } array;
        struct { mcc_type_t *ret; mcc_type_t **params; int num_params; } function;
        struct { const char *tag; mcc_struct_field_t *fields; int num_fields; } record;
    } data;
} mcc_type_t;
```

## Symbol Table

The symbol table uses a hash table with scope chaining:

```c
typedef struct mcc_symbol {
    const char *name;
    mcc_type_t *type;
    mcc_symbol_kind_t kind;  /* SYM_VAR, SYM_FUNC, SYM_TYPE, SYM_ENUM_CONST */
    int scope_level;
    struct mcc_symbol *next; /* Hash chain */
} mcc_symbol_t;

typedef struct mcc_symtab {
    mcc_symbol_t **buckets;
    size_t num_buckets;
    int current_scope;
} mcc_symtab_t;
```

## AST Node Types

The AST has 40+ node types organized by category:

### Declarations
- `AST_TRANS_UNIT`: Translation unit (list of declarations)
- `AST_FUNC_DECL`: Function declaration/definition
- `AST_VAR_DECL`: Variable declaration
- `AST_PARAM_DECL`: Function parameter
- `AST_STRUCT_DECL`, `AST_UNION_DECL`, `AST_ENUM_DECL`
- `AST_TYPEDEF_DECL`

### Statements
- `AST_COMPOUND_STMT`: Block `{ ... }`
- `AST_EXPR_STMT`: Expression statement
- `AST_IF_STMT`, `AST_WHILE_STMT`, `AST_DO_STMT`, `AST_FOR_STMT`
- `AST_SWITCH_STMT`, `AST_CASE_STMT`, `AST_DEFAULT_STMT`
- `AST_RETURN_STMT`, `AST_BREAK_STMT`, `AST_CONTINUE_STMT`
- `AST_GOTO_STMT`, `AST_LABEL_STMT`

### Expressions
- `AST_INT_LIT`, `AST_FLOAT_LIT`, `AST_CHAR_LIT`, `AST_STRING_LIT`
- `AST_IDENT_EXPR`: Identifier reference
- `AST_BINARY_EXPR`: Binary operation
- `AST_UNARY_EXPR`: Unary operation
- `AST_TERNARY_EXPR`: Conditional `?:`
- `AST_CALL_EXPR`: Function call
- `AST_SUBSCRIPT_EXPR`: Array subscript `a[i]`
- `AST_MEMBER_EXPR`: Member access `.` and `->`
- `AST_CAST_EXPR`: Type cast
- `AST_SIZEOF_EXPR`: sizeof operator

## Error Handling

Errors are reported through the compiler context:

```c
void mcc_error(mcc_context_t *ctx, const char *fmt, ...);
void mcc_error_at(mcc_context_t *ctx, mcc_location_t loc, const char *fmt, ...);
void mcc_warning(mcc_context_t *ctx, const char *fmt, ...);
void mcc_warning_at(mcc_context_t *ctx, mcc_location_t loc, const char *fmt, ...);
```

The context tracks error/warning counts and can be configured to treat warnings as errors.

## ANVIL Integration

MCC uses ANVIL for code generation:

1. Create ANVIL context and module
2. For each function:
   - Create ANVIL function with type signature
   - Generate ANVIL IR for function body
   - Use ANVIL builder API for instructions
3. Call `anvil_module_codegen()` to generate target assembly

Key ANVIL APIs used:
- `anvil_build_alloca()`: Stack allocation
- `anvil_build_load()`, `anvil_build_store()`: Memory access
- `anvil_build_add()`, `anvil_build_sub()`, etc.: Arithmetic
- `anvil_build_icmp()`: Integer comparison
- `anvil_build_br()`, `anvil_build_cond_br()`: Control flow
- `anvil_build_call()`: Function calls
- `anvil_build_ret()`: Return
- `anvil_build_struct_gep()`: Struct field access

## Extending MCC

### Adding a new operator

1. Add token type in `include/token.h`
2. Add lexer recognition in `src/lexer/lex_operator.c`
3. Add parser handling in `src/parser/parse_expr.c`
4. Add type checking in `src/sema.c`
5. Add code generation in `src/codegen.c`

### Adding a new statement

1. Add AST node type in `include/ast.h`
2. Add parser function in `src/parser/parse_stmt.c`
3. Add semantic analysis in `src/sema.c`
4. Add code generation in `src/codegen.c`

### Adding a new expression

1. Add AST node type in `include/ast.h`
2. Add parser function in `src/parser/parse_expr.c`
3. Add semantic analysis in `src/sema.c`
4. Add code generation in `src/codegen.c`

### Adding a new type

1. Add type kind in `include/types.h`
2. Add type parsing in `src/parser/parse_type.c`
3. Add type creation function in `src/types.c`
4. Add type checking rules in `src/sema.c`
5. Add code generation support in `src/codegen.c`
