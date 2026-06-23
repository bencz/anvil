# MCC Semantic Analysis Documentation

This document describes the semantic analysis component of MCC.

## Overview

Semantic analysis validates the AST and performs:
- Type checking
- Symbol resolution
- Scope management
- Implicit type conversions
- Error detection
- C Standard feature validation

## Module Structure

The semantic analyzer is organized into modular files in `src/sema/`:

| File | Description |
|------|-------------|
| `sema_internal.h` | Internal header with structures, function declarations, and C standard feature checks |
| `sema.c` | Main module - public API and entry points |
| `sema_expr.c` | Expression analysis (literals, identifiers, binary/unary ops, calls, member access) |
| `sema_stmt.c` | Statement analysis (if, while, for, switch, return, break, continue, goto) |
| `sema_decl.c` | Declaration analysis (functions, variables, typedefs, structs, enums) - registers tags in symbol table |
| `sema_type.c` | Type checking utilities (lvalue checks, assignment compatibility, type conversions) |
| `sema_const.c` | Constant expression evaluation (for array sizes, case labels, static_assert) |
| `sema_dump.c` | Dump functions for debugging (symbol table, scopes, AST declarations with full details) |

## Semantic Analyzer Structure

```c
struct mcc_sema {
    mcc_context_t *ctx;
    mcc_symtab_t *symtab;
    mcc_type_context_t *types;

    /* Current function context */
    mcc_symbol_t *current_func;
    mcc_type_t *current_return_type;

    /* Loop/switch nesting for break/continue/case validation */
    int loop_depth;
    int switch_depth;

    /* Label tracking (forward goto resolution) */
    mcc_symbol_t **pending_gotos;
    size_t num_pending_gotos;
};
```

`break`/`continue` and `case`/`default` placement are validated against the
`loop_depth`/`switch_depth` counters (incremented when entering loops/switches),
not boolean flags.

## C Standard Feature Checks

The semantic analyzer includes helper functions for checking C standard features:

```c
/* Check if a feature is enabled */
static inline bool sema_has_feature(mcc_sema_t *sema, mcc_feature_id_t feat);

/* C89: Implicit int return type (deprecated in C99, removed in C11) */
static inline bool sema_has_implicit_int(mcc_sema_t *sema);

/* C89: Implicit function declarations (removed in C99) */
static inline bool sema_has_implicit_func_decl(mcc_sema_t *sema);

/* C99: Variable Length Arrays */
static inline bool sema_has_vla(mcc_sema_t *sema);

/* C11: _Static_assert */
static inline bool sema_has_static_assert(mcc_sema_t *sema);

/* C11: _Generic selection */
static inline bool sema_has_generic(mcc_sema_t *sema);

/* C23: nullptr constant */
static inline bool sema_has_nullptr(mcc_sema_t *sema);
```

## Symbol Table

The symbol table tracks all declared identifiers:

```c
typedef enum {
    SYM_VAR,            /* Variable */
    SYM_FUNC,           /* Function */
    SYM_PARAM,          /* Function parameter */
    SYM_TYPEDEF,        /* Typedef name */
    SYM_STRUCT,         /* Struct tag */
    SYM_UNION,          /* Union tag */
    SYM_ENUM,           /* Enum tag */
    SYM_ENUM_CONST,     /* Enum constant */
    SYM_LABEL,          /* Label (goto target) */
    SYM_COUNT
} mcc_sym_kind_t;

struct mcc_symbol {
    mcc_sym_kind_t kind;
    const char *name;
    mcc_type_t *type;
    mcc_location_t location;        /* Where the symbol was declared */
    mcc_storage_class_t storage;

    /* Variables: stack offset or global name; enum constants: value */
    union {
        int stack_offset;
        const char *global_name;
        int enum_value;
    } data;

    bool is_defined;                /* Has a definition (vs just a declaration) */
    bool is_used;                   /* Has been referenced */
    bool is_parameter;              /* Is a function parameter */

    mcc_ast_node_t *ast_node;       /* For functions with bodies */
    mcc_symbol_t *next;             /* Hash chain */
};
```

The symbol table is **scope-chain based**, not a single global hash table. Each scope
owns three separate namespaces — ordinary symbols, tags (struct/union/enum), and labels:

```c
struct mcc_scope {
    mcc_scope_t *parent;            /* Enclosing scope */

    mcc_symbol_t **symbols;         /* Ordinary symbol hash table */
    size_t table_size, num_symbols;

    mcc_symbol_t **tags;            /* struct/union/enum tag namespace */
    size_t tag_table_size, num_tags;

    mcc_symbol_t **labels;          /* Label namespace (function scope only) */
    size_t label_table_size, num_labels;

    int depth;                      /* Nesting depth */
    bool is_file_scope;             /* Global scope */
    bool is_function_scope;         /* Function body scope */
    bool is_block_scope;            /* Block scope */
    int stack_offset;               /* Current stack offset for locals */
};

struct mcc_symtab {
    mcc_context_t *ctx;
    mcc_scope_t *current;           /* Current (innermost) scope */
    mcc_scope_t *global;            /* Global/file scope */
    mcc_type_context_t *types;
};
```

## Scope Management

Scopes form a linked chain. Pushing a scope allocates a new `mcc_scope_t` whose
`parent` points at the previous scope; popping restores the parent. Lookups walk the
chain outward from the current scope.

```c
/* Enter a block scope / function-body scope; leave the current scope */
void mcc_symtab_push_scope(mcc_symtab_t *symtab);
void mcc_symtab_push_function_scope(mcc_symtab_t *symtab);  /* also enables the label namespace */
void mcc_symtab_pop_scope(mcc_symtab_t *symtab);

mcc_scope_t *mcc_symtab_current_scope(mcc_symtab_t *symtab);
bool mcc_symtab_is_global_scope(mcc_symtab_t *symtab);
```

## Symbol Operations

```c
/* Define a new symbol in the current scope */
mcc_symbol_t *mcc_symtab_define(mcc_symtab_t *symtab, const char *name,
                                 mcc_sym_kind_t kind, mcc_type_t *type,
                                 mcc_location_t loc);

/* Look up a symbol (searches the scope chain outward) */
mcc_symbol_t *mcc_symtab_lookup(mcc_symtab_t *symtab, const char *name);

/* Look up in the current scope only */
mcc_symbol_t *mcc_symtab_lookup_current(mcc_symtab_t *symtab, const char *name);

/* Tag namespace (struct/union/enum) */
mcc_symbol_t *mcc_symtab_define_tag(mcc_symtab_t *symtab, const char *name,
                                     mcc_sym_kind_t kind, mcc_type_t *type,
                                     mcc_location_t loc);
mcc_symbol_t *mcc_symtab_lookup_tag(mcc_symtab_t *symtab, const char *name);
mcc_symbol_t *mcc_symtab_lookup_tag_current(mcc_symtab_t *symtab, const char *name);

/* Label namespace (function scope) */
mcc_symbol_t *mcc_symtab_define_label(mcc_symtab_t *symtab, const char *name,
                                       mcc_location_t loc);
mcc_symbol_t *mcc_symtab_lookup_label(mcc_symtab_t *symtab, const char *name);

/* Typedef check used by the parser */
bool mcc_symtab_is_typedef(mcc_symtab_t *symtab, const char *name);
```

## Semantic Analysis API

```c
/* Create and destroy */
mcc_sema_t *mcc_sema_create(mcc_context_t *ctx);
void mcc_sema_destroy(mcc_sema_t *sema);

/* Main entry point */
bool mcc_sema_analyze(mcc_sema_t *sema, mcc_ast_node_t *ast);
```

## Expression Analysis

Each expression node is analyzed to determine its type:

```c
static mcc_type_t *analyze_expr(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    switch (expr->kind) {
        case AST_INT_LIT:
            expr->type = mcc_type_int(sema->types);
            return expr->type;
            
        case AST_IDENT_EXPR: {
            mcc_symbol_t *sym = mcc_symtab_lookup(sema->symtab, 
                                                   expr->data.ident_expr.name);
            if (!sym) {
                mcc_error_at(sema->ctx, expr->location,
                             "Undeclared identifier '%s'", 
                             expr->data.ident_expr.name);
                return NULL;
            }
            expr->data.ident_expr.symbol = sym;
            expr->type = sym->type;
            return expr->type;
        }
        
        case AST_BINARY_EXPR: {
            mcc_type_t *lhs = analyze_expr(sema, expr->data.binary_expr.lhs);
            mcc_type_t *rhs = analyze_expr(sema, expr->data.binary_expr.rhs);
            if (!lhs || !rhs) return NULL;
            
            /* Type check based on operator */
            switch (expr->data.binary_expr.op) {
                case BINOP_ADD:
                case BINOP_SUB:
                    /* Arithmetic or pointer arithmetic */
                    expr->type = mcc_type_common(sema->types, lhs, rhs);
                    break;
                case BINOP_EQ:
                case BINOP_NE:
                case BINOP_LT:
                case BINOP_GT:
                    /* Comparison - result is int */
                    expr->type = mcc_type_int(sema->types);
                    break;
                case BINOP_ASSIGN:
                    /* Check lvalue */
                    /* Check type compatibility */
                    expr->type = lhs;
                    break;
                /* ... */
            }
            return expr->type;
        }
        
        case AST_CALL_EXPR: {
            mcc_type_t *func_type = analyze_expr(sema, expr->data.call_expr.func);
            if (!func_type) return NULL;
            
            if (!mcc_type_is_function(func_type)) {
                mcc_error_at(sema->ctx, expr->location,
                             "Called object is not a function");
                return NULL;
            }
            
            /* Check argument count and types */
            /* ... */
            
            expr->type = func_type->data.function.return_type;
            return expr->type;
        }
        
        case AST_MEMBER_EXPR: {
            mcc_type_t *obj_type = analyze_expr(sema, expr->data.member_expr.object);
            if (!obj_type) return NULL;
            
            if (expr->data.member_expr.is_arrow) {
                if (!mcc_type_is_pointer(obj_type)) {
                    mcc_error_at(sema->ctx, expr->location,
                                 "Member reference type is not a pointer");
                    return NULL;
                }
                obj_type = obj_type->data.pointer.pointee;
            }
            
            if (!mcc_type_is_record(obj_type)) {
                mcc_error_at(sema->ctx, expr->location,
                             "Member reference base type is not a struct or union");
                return NULL;
            }
            
            mcc_struct_field_t *field = mcc_type_find_field(obj_type, 
                                                            expr->data.member_expr.member);
            if (!field) {
                mcc_error_at(sema->ctx, expr->location,
                             "No member named '%s'", expr->data.member_expr.member);
                return NULL;
            }
            
            expr->type = field->type;
            return expr->type;
        }
        
        /* ... other expression types */
    }
}
```

## Statement Analysis

```c
static void analyze_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    switch (stmt->kind) {
        case AST_COMPOUND_STMT:
            mcc_symtab_push_scope(sema->symtab);
            for (size_t i = 0; i < stmt->data.compound_stmt.num_stmts; i++) {
                analyze_stmt(sema, stmt->data.compound_stmt.stmts[i]);
            }
            mcc_symtab_pop_scope(sema->symtab);
            break;
            
        case AST_IF_STMT: {
            mcc_type_t *cond_type = analyze_expr(sema, stmt->data.if_stmt.cond);
            if (cond_type && !mcc_type_is_scalar(cond_type)) {
                mcc_error_at(sema->ctx, stmt->location,
                             "Condition must be scalar type");
            }
            analyze_stmt(sema, stmt->data.if_stmt.then_stmt);
            if (stmt->data.if_stmt.else_stmt) {
                analyze_stmt(sema, stmt->data.if_stmt.else_stmt);
            }
            break;
        }
        
        case AST_WHILE_STMT: {
            mcc_type_t *cond_type = analyze_expr(sema, stmt->data.while_stmt.cond);
            if (cond_type && !mcc_type_is_scalar(cond_type)) {
                mcc_error_at(sema->ctx, stmt->location,
                             "Condition must be scalar type");
            }
            sema->loop_depth++;
            analyze_stmt(sema, stmt->data.while_stmt.body);
            sema->loop_depth--;
            break;
        }
        
        case AST_RETURN_STMT: {
            if (stmt->data.return_stmt.value) {
                mcc_type_t *val_type = analyze_expr(sema, stmt->data.return_stmt.value);
                if (mcc_type_is_void(sema->current_return_type)) {
                    mcc_error_at(sema->ctx, stmt->location,
                                 "Void function should not return a value");
                }
                /* Check type compatibility */
            } else {
                if (!mcc_type_is_void(sema->current_return_type)) {
                    mcc_error_at(sema->ctx, stmt->location,
                                 "Non-void function should return a value");
                }
            }
            break;
        }
        
        case AST_BREAK_STMT:
            if (sema->loop_depth == 0 && sema->switch_depth == 0) {
                mcc_error_at(sema->ctx, stmt->location,
                             "break statement outside of loop or switch");
            }
            break;
            
        case AST_CONTINUE_STMT:
            if (sema->loop_depth == 0) {
                mcc_error_at(sema->ctx, stmt->location,
                             "continue statement outside of loop");
            }
            break;
            
        /* ... other statement types */
    }
}
```

## Declaration Analysis

```c
static void analyze_decl(mcc_sema_t *sema, mcc_ast_node_t *decl)
{
    switch (decl->kind) {
        case AST_VAR_DECL: {
            /* Check for redefinition (current scope only); redefinition is
               reported inside mcc_symtab_define itself */
            mcc_symbol_t *existing = mcc_symtab_lookup_current(sema->symtab,
                                                               decl->data.var_decl.name);
            (void)existing;

            /* Define symbol: (symtab, name, kind, type, location) */
            mcc_symtab_define(sema->symtab, decl->data.var_decl.name,
                              SYM_VAR, decl->data.var_decl.var_type, decl->location);
            
            /* Analyze initializer */
            if (decl->data.var_decl.init) {
                mcc_type_t *init_type = analyze_expr(sema, decl->data.var_decl.init);
                /* Check type compatibility */
            }
            break;
        }
        
        case AST_FUNC_DECL: {
            /* Define function symbol */
            mcc_type_t *func_type = /* build function type */;
            mcc_symtab_define(sema->symtab, decl->data.func_decl.name,
                              SYM_FUNC, func_type, decl->location);
            
            if (decl->data.func_decl.body) {
                /* Enter function scope (enables the label namespace) */
                mcc_symtab_push_function_scope(sema->symtab);
                
                /* Define parameters */
                for (size_t i = 0; i < decl->data.func_decl.num_params; i++) {
                    mcc_ast_node_t *param = decl->data.func_decl.params[i];
                    mcc_symtab_define(sema->symtab, param->data.param_decl.name,
                                      SYM_PARAM, param->data.param_decl.param_type,
                                      param->location);
                }
                
                /* Set return type context */
                sema->current_return_type = decl->data.func_decl.return_type;
                
                /* Analyze body */
                analyze_stmt(sema, decl->data.func_decl.body);
                
                mcc_symtab_pop_scope(sema->symtab);
            }
            break;
        }
        
        /* ... other declaration types */
    }
}
```

## Type Checking Rules

### Assignment

```c
/* Valid assignments:
   - Same type
   - Arithmetic to arithmetic (with conversion)
   - Pointer to pointer of compatible type
   - Pointer to/from void*
   - Pointer to/from integer (with warning)
   - NULL to any pointer
*/
```

### Arithmetic Operations

```c
/* Both operands must be arithmetic types.
   Result type is the common type after usual arithmetic conversions. */
```

### Comparison Operations

```c
/* Valid comparisons:
   - Both arithmetic
   - Both pointers to compatible types
   - Pointer and NULL
   Result type is always int. */
```

### Pointer Arithmetic

```c
/* Valid:
   - pointer + integer
   - integer + pointer
   - pointer - integer
   - pointer - pointer (same type, result is ptrdiff_t)
*/
```

### Function Calls

```c
/* Check:
   - Callee is function or pointer to function
   - Argument count matches (unless variadic)
   - Argument types are compatible with parameter types
*/
```

### Member Access

```c
/* For expr.member:
   - expr must be struct or union type
   - member must exist in the struct/union

   For expr->member:
   - expr must be pointer to struct or union
   - member must exist in the pointed-to struct/union
*/
```

## Error Messages

The semantic analyzer produces descriptive error messages:

```
test.c:10:5: error: Undeclared identifier 'foo'
test.c:15:12: error: Called object is not a function
test.c:20:5: error: No member named 'bar'
test.c:25:5: error: Break statement not within loop or switch
test.c:30:5: error: Incompatible types in assignment
test.c:35:5: warning: Implicit conversion from 'int' to 'char'
```

## Usage Example

```c
mcc_context_t *ctx = mcc_context_create();
mcc_sema_t *sema = mcc_sema_create(ctx);

/* Parse source to get AST */
mcc_ast_node_t *ast = /* ... */;

/* Run semantic analysis */
if (mcc_sema_analyze(sema, ast)) {
    /* AST is valid, proceed to code generation */
    /* sema->symtab contains all symbols */
    /* sema->types contains type information */
}

mcc_sema_destroy(sema);
mcc_context_destroy(ctx);
```

## Recent Fixes

### Enum Constants Registration

Enum constants are now properly registered in the symbol table:

```c
/* In sema_decl.c - analyze_enum_decl() */
for (mcc_enum_const_t *c = enum_type->data.enumeration.constants; c; c = c->next) {
    mcc_symbol_t *sym = mcc_symtab_define(sema->symtab,
        c->name, SYM_ENUM_CONST, int_type, decl->location);
    if (sym) {
        sym->data.enum_value = (int)c->value;
    }
}
```

This also applies to `typedef enum { ... } Name;` declarations.

### AST_DECL_LIST Support

Multiple variable declarations (`int a, b, c;`) are now properly analyzed:

```c
/* In sema_stmt.c - sema_analyze_compound_stmt() */
if (s->kind == AST_VAR_DECL || s->kind == AST_FUNC_DECL || s->kind == AST_DECL_LIST) {
    sema_analyze_decl(sema, s);
}
```

### C99 For Loop Declarations

For loops with declarations (`for (int i = 0; ...)`) now check `init_decl`:

```c
/* In sema_stmt.c - sema_analyze_for_stmt() */
if (stmt->data.for_stmt.init_decl) {
    sema_analyze_decl(sema, stmt->data.for_stmt.init_decl);
} else if (stmt->data.for_stmt.init) {
    /* ... */
}
```

### Integer Type Checking

`TYPE_LONG_LONG` and `TYPE_BOOL` are now recognized as integer types:

```c
/* In types.c - mcc_type_is_integer() */
case TYPE_LONG_LONG:
case TYPE_BOOL:
    return true;
```

### C99 `__func__` Support

The predefined identifier `__func__` is recognized and returns `const char*`:

```c
/* In sema_expr.c - analyze_ident_expr() */
if (strcmp(name, "__func__") == 0) {
    mcc_type_t *char_type = mcc_type_char(sema->types);
    char_type->qualifiers |= QUAL_CONST;
    expr->type = mcc_type_pointer(sema->types, char_type);
    expr->data.ident_expr.is_func_name = true;
    return expr->type;
}
```

### C99 VLA Support

Variable Length Arrays are recognized as complete types in C99:

```c
/* In sema_type.c - sema_check_complete_type() */
if (mcc_type_is_array(type) && type->data.array.length == 0) {
    if (type->data.array.is_vla && sema_has_vla(sema)) {
        return true;  /* VLA is complete in C99 */
    }
    return false;
}
```

### Case Expression Analysis

Case expressions are now properly analyzed to resolve symbols (like enum constants):

```c
/* In sema_stmt.c - analyze_case_stmt() */
static bool analyze_case_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    /* Analyze case expression to resolve symbols (e.g., enum constants) */
    sema_analyze_expr(sema, stmt->data.case_stmt.expr);
    
    /* Case expression must be constant */
    int64_t case_val;
    if (!sema_eval_const_expr(sema, stmt->data.case_stmt.expr, &case_val)) {
        mcc_error_at(sema->ctx, stmt->location,
                     "case expression is not a constant");
    }
    
    sema_analyze_stmt(sema, stmt->data.case_stmt.stmt);
    return true;
}
```

### Function-to-Pointer Assignment

Functions can now be assigned to function pointer variables (function decays to pointer):

```c
/* In sema_type.c - sema_check_assignment_compat() */
/* Function can be assigned to pointer-to-function (function decays to pointer) */
if (mcc_type_is_pointer(lhs) && rhs->kind == TYPE_FUNCTION) {
    mcc_type_t *lhs_pointee = lhs->data.pointer.pointee;
    if (lhs_pointee && lhs_pointee->kind == TYPE_FUNCTION) {
        /* Check if function signatures are compatible */
        if (mcc_type_is_compatible(lhs_pointee, rhs)) {
            return true;
        }
        mcc_warning_at(sema->ctx, loc,
                       "incompatible function pointer types in assignment");
        return true;
    }
}
```

### Anonymous Field Lookup

The `mcc_type_find_field` function skips anonymous bitfield padding but recurses into
anonymous struct/union members (C11), so a member can be found through the outer type:

```c
/* In types.c - mcc_type_find_field() */
mcc_struct_field_t *mcc_type_find_field(mcc_type_t *type, const char *name)
{
    for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next) {
        if (f->name) {
            if (strcmp(f->name, name) == 0) return f;
            continue;
        }
        /* Anonymous struct/union member: recurse; bitfield padding bails out */
        if (f->type && (f->type->kind == TYPE_STRUCT || f->type->kind == TYPE_UNION)) {
            mcc_struct_field_t *inner = mcc_type_find_field(f->type, name);
            if (inner) return inner;
        }
    }
    return NULL;
}
```

### Struct/Union/Enum Tag Registration

The semantic analyzer now properly registers struct, union, and enum tags in the symbol table:

```c
/* In sema_decl.c - analyze_struct_decl() */
static bool analyze_struct_decl(mcc_sema_t *sema, mcc_ast_node_t *decl, bool is_union)
{
    if (decl->data.struct_decl.tag && decl->data.struct_decl.struct_type) {
        mcc_symtab_define_tag(sema->symtab,
            decl->data.struct_decl.tag,
            is_union ? SYM_UNION : SYM_STRUCT,
            decl->data.struct_decl.struct_type,
            decl->location);
    }
    return true;
}
```

## Semantic Dump Functions

The semantic analyzer provides several dump functions for debugging:

### Command Line Options

| Option | Description |
|--------|-------------|
| `-dump-sema` | Standard dump with organized information |
| `-dump-sema-verbose` | Complete detailed dump with all information |

### Dump Functions API

```c
/* Standard dump - symbol table and AST declarations */
void mcc_sema_dump(mcc_sema_t *sema, FILE *out);

/* Full dump with AST traversal for local variables */
void mcc_sema_dump_full(mcc_sema_t *sema, mcc_ast_node_t *ast, FILE *out);

/* Verbose dump - everything in detailed format */
void mcc_sema_dump_verbose(mcc_sema_t *sema, mcc_ast_node_t *ast, FILE *out);

/* Symbol table only */
void mcc_sema_dump_symtab(mcc_sema_t *sema, FILE *out);

/* Global-scope symbols only */
void mcc_sema_dump_globals(mcc_sema_t *sema, FILE *out);
```

### Information Dumped

The dump includes:

1. **Type Context**
   - Pointer size for target architecture
   - Primitive type sizes and alignments

2. **Symbol Table (Global Scope)**
   - Functions (with definition/used flags)
   - Global variables
   - Typedefs
   - Enum constants (with values)
   - Tags (struct/union/enum with fields/constants)

3. **Complete Symbol Information** (verbose mode)
   - Location (file:line:column)
   - Storage class (auto, static, extern, register, typedef)
   - Flags (defined, used, parameter)
   - Stack offset (for local variables)
   - Global name (for functions/globals)

4. **Complete Type Information** (verbose mode)
   - Qualifiers (const, volatile, restrict, _Atomic)
   - Size and alignment
   - Unsigned flag
   - Function specifiers (inline, _Noreturn)
   - Type-specific details:
     - Pointer: pointee type
     - Array: element type, length, VLA/flexible flags
     - Function: return type, parameters, variadic/K&R flags
     - Struct/Union: tag, completeness, fields with offsets
     - Enum: tag, completeness, constants with values

5. **AST Declarations**
   - All declarations with full details
   - Local variables in function bodies
   - Parameters
   - Nested scopes (for loops, if blocks, etc.)

6. **C23 Attributes**
   - `[[deprecated]]` with optional message
   - `[[nodiscard]]` with optional message
   - `[[maybe_unused]]`
   - `[[noreturn]]`
   - `[[fallthrough]]`
   - GNU attributes (`packed`, `aligned`, `pure`, `const`, `unused`)

### Example Output

```
=== Full Semantic Analysis Dump ===

C Standard: c23 (ISO/IEC 9899:2024)

=== Type Context ===

Pointer size: 8 bytes

Primitive Types:
  char:        size=1, align=1
  int:         size=4, align=4
  long:        size=8, align=8

=== Global Scope ===

Functions:
  Function 'main' 'int ()' defined </file.c:10:1>

Tags (struct/union/enum):
  Struct 'Point' 'struct Point' </file.c:1:1>
    Fields:
      'x' 'int' [offset: 0, size: 4]
      'y' 'int' [offset: 4, size: 4]

=== Declarations (from AST) ===

Function 'compute' 'int' definition </file.c:5:1>
  Attributes:
    [[nodiscard]]
  Parameters: (2)
    Parameter [0] 'a' 'int' </file.c:5:1>
    Parameter [1] 'b' 'int' </file.c:5:1>
  Body:
    Block:
      Variable 'result' 'int' </file.c:6:5> initialized
```
