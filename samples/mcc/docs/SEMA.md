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
| `sema_decl.c` | Declaration analysis (functions, variables, typedefs, structs, enums) |
| `sema_type.c` | Type checking utilities (lvalue checks, assignment compatibility, type conversions) |
| `sema_const.c` | Constant expression evaluation (for array sizes, case labels, static_assert) |

## Semantic Analyzer Structure

```c
typedef struct mcc_sema {
    mcc_context_t *ctx;
    mcc_symtab_t *symtab;
    mcc_type_context_t *types;
    
    /* Current function context */
    mcc_symbol_t *current_func;
    mcc_type_t *current_return_type;
    int loop_depth;             /* For break/continue validation */
    int switch_depth;           /* For case/default validation */
} mcc_sema_t;
```

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
    SYM_VAR,        /* Variable */
    SYM_FUNC,       /* Function */
    SYM_TYPE,       /* typedef name */
    SYM_ENUM_CONST, /* Enumeration constant */
    SYM_LABEL       /* goto label */
} mcc_symbol_kind_t;

typedef struct mcc_symbol {
    const char *name;
    mcc_type_t *type;
    mcc_symbol_kind_t kind;
    mcc_storage_class_t storage;
    int scope_level;
    mcc_location_t def_loc;
    struct mcc_symbol *next;    /* Hash chain */
} mcc_symbol_t;

typedef struct mcc_symtab {
    mcc_context_t *ctx;
    mcc_symbol_t **buckets;
    size_t num_buckets;
    int current_scope;
} mcc_symtab_t;
```

## Scope Management

Scopes are managed with a scope level counter:

```c
void mcc_symtab_push_scope(mcc_symtab_t *tab)
{
    tab->current_scope++;
}

void mcc_symtab_pop_scope(mcc_symtab_t *tab)
{
    /* Remove all symbols at current scope */
    for (size_t i = 0; i < tab->num_buckets; i++) {
        mcc_symbol_t **pp = &tab->buckets[i];
        while (*pp) {
            if ((*pp)->scope_level == tab->current_scope) {
                *pp = (*pp)->next;
            } else {
                pp = &(*pp)->next;
            }
        }
    }
    tab->current_scope--;
}
```

## Symbol Operations

```c
/* Define a new symbol */
mcc_symbol_t *mcc_symtab_define(mcc_symtab_t *tab, const char *name,
                                 mcc_type_t *type, mcc_symbol_kind_t kind);

/* Look up a symbol (searches all scopes) */
mcc_symbol_t *mcc_symtab_lookup(mcc_symtab_t *tab, const char *name);

/* Look up in current scope only */
mcc_symbol_t *mcc_symtab_lookup_local(mcc_symtab_t *tab, const char *name);
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
            bool old_in_loop = sema->in_loop;
            sema->in_loop = true;
            analyze_stmt(sema, stmt->data.while_stmt.body);
            sema->in_loop = old_in_loop;
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
            if (!sema->in_loop && !sema->in_switch) {
                mcc_error_at(sema->ctx, stmt->location,
                             "Break statement not within loop or switch");
            }
            break;
            
        case AST_CONTINUE_STMT:
            if (!sema->in_loop) {
                mcc_error_at(sema->ctx, stmt->location,
                             "Continue statement not within loop");
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
            /* Check for redefinition */
            mcc_symbol_t *existing = mcc_symtab_lookup_local(sema->symtab,
                                                              decl->data.var_decl.name);
            if (existing) {
                mcc_error_at(sema->ctx, decl->location,
                             "Redefinition of '%s'", decl->data.var_decl.name);
            }
            
            /* Define symbol */
            mcc_symtab_define(sema->symtab, decl->data.var_decl.name,
                              decl->data.var_decl.var_type, SYM_VAR);
            
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
                              func_type, SYM_FUNC);
            
            if (decl->data.func_decl.body) {
                /* Enter function scope */
                mcc_symtab_push_scope(sema->symtab);
                
                /* Define parameters */
                for (size_t i = 0; i < decl->data.func_decl.num_params; i++) {
                    mcc_ast_node_t *param = decl->data.func_decl.params[i];
                    mcc_symtab_define(sema->symtab, param->data.param_decl.name,
                                      param->data.param_decl.param_type, SYM_VAR);
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
