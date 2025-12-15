# MCC Architecture

This document describes the internal architecture of the MCC (Micro C Compiler) for developers who want to understand or extend the compiler.

## Overview

MCC is a complete C89 compiler frontend that generates code through the ANVIL intermediate representation. The compiler follows a traditional multi-pass architecture:

```
Source Code → Preprocessor → Lexer → Parser → Semantic Analysis → Code Generation → Assembly
```

## Compilation Pipeline

### 1. Preprocessor (`preprocessor.c`)

The preprocessor handles all `#` directives before parsing:

- **Macro expansion**: Object-like (`#define FOO 1`) and function-like (`#define MAX(a,b) ...`)
- **Conditional compilation**: `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`
- **File inclusion**: `#include <file>` and `#include "file"`
- **Other directives**: `#undef`, `#error`, `#warning`, `#line`, `#pragma`

**Key data structures:**
- `mcc_preprocessor_t`: Main preprocessor state
- `mcc_macro_t`: Macro definition (name, parameters, body tokens)
- `mcc_cond_stack_t`: Stack for nested conditional compilation
- `mcc_include_file_t`: Stack for nested includes

### 2. Lexer (`lexer.c`)

The lexer converts source text into tokens:

- **Keywords**: `int`, `if`, `while`, `struct`, etc. (32 C89 keywords)
- **Identifiers**: Variable and function names
- **Literals**: Integer, floating-point, character, string
- **Operators**: `+`, `-`, `*`, `/`, `==`, `!=`, `->`, etc.
- **Punctuation**: `{`, `}`, `(`, `)`, `;`, `,`, etc.

**Key data structures:**
- `mcc_lexer_t`: Lexer state (source, position, line/column)
- `mcc_token_t`: Token with type, text, location, literal value

### 3. Parser (`parser.c`)

The parser builds an Abstract Syntax Tree (AST) using recursive descent:

- **Declarations**: Variables, functions, structs, unions, enums, typedefs
- **Statements**: if, while, for, do-while, switch, return, goto, labels
- **Expressions**: All C89 operators with correct precedence
- **Typedef handling**: Registers typedef names and recognizes them as types

**Key data structures:**
- `mcc_parser_t`: Parser state
- `mcc_ast_node_t`: AST node (40+ node types)
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

1. Add token type in `token.h`
2. Add lexer recognition in `lexer.c`
3. Add parser handling in `parser.c`
4. Add type checking in `sema.c`
5. Add code generation in `codegen.c`

### Adding a new statement

1. Add AST node type in `ast.h`
2. Add parser function in `parser.c`
3. Add semantic analysis in `sema.c`
4. Add code generation in `codegen.c`

### Adding a new type

1. Add type kind in `types.h`
2. Add type creation function in `types.c`
3. Add type checking rules in `sema.c`
4. Add code generation support in `codegen.c`
