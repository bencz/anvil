# MCC Code Generator Documentation

This document describes the code generator component of MCC.

## Overview

The code generator translates the AST into ANVIL IR, which is then lowered to target-specific assembly. This two-stage approach allows MCC to target multiple architectures without duplicating code generation logic.

## File Organization

The code generator is organized into modular files in `src/codegen/`:

| File | Description |
|------|-------------|
| `codegen_internal.h` | Internal header with structures and function declarations |
| `codegen.c` | Main module - public API, local/string/label/function management |
| `codegen_type.c` | Type conversion from MCC types to ANVIL types |
| `codegen_expr.c` | Expression code generation (literals, operators, calls, casts) |
| `codegen_stmt.c` | Statement code generation (if, while, for, switch, return) |
| `codegen_decl.c` | Declaration code generation (functions, global variables) |

### Module Responsibilities

**codegen.c** (Main Module)
- Public API: `mcc_codegen_create()`, `mcc_codegen_destroy()`, `mcc_codegen_generate()`
- Architecture mapping: `codegen_mcc_to_anvil_arch()`
- Local variable management: `codegen_find_local()`, `codegen_add_local()`
- String literal pool: `codegen_get_string_literal()`
- Label management: `codegen_get_label_block()`
- Function table: `codegen_find_func()`, `codegen_add_func()`, `codegen_get_or_declare_func()`
- Block management: `codegen_set_current_block()`, `codegen_block_has_terminator()`

**codegen_type.c** (Type Conversion)
- `codegen_type()`: Convert MCC type to ANVIL type
- Handles all type kinds: void, char, short, int, long, float, double, pointer, array, struct, union, function

**codegen_expr.c** (Expression Generation)
- `codegen_expr()`: Generate code for any expression, returns `anvil_value_t*`
- `codegen_lvalue()`: Generate code for lvalue (returns pointer)
- Handles: literals, identifiers, binary/unary operators, ternary, calls, subscript, member access, casts, sizeof, comma

**codegen_stmt.c** (Statement Generation)
- `codegen_stmt()`: Generate code for any statement
- `codegen_compound_stmt()`: Compound statement `{ ... }`
- `codegen_if_stmt()`, `codegen_while_stmt()`, `codegen_do_stmt()`, `codegen_for_stmt()`
- `codegen_switch_stmt()`, `codegen_return_stmt()`
- Handles: break, continue, goto, labels, local variable declarations

**codegen_decl.c** (Declaration Generation)
- `codegen_decl()`: Generate code for any declaration
- `codegen_func()`: Generate code for function definition
- `codegen_global_var()`: Generate code for global variable

## ANVIL Integration

MCC uses the ANVIL library for:
1. **IR construction**: Building SSA-form intermediate representation
2. **Type representation**: Mapping C types to ANVIL types
3. **Code generation**: Lowering IR to target assembly

### Supported Targets

| Architecture | Output Format | Description |
|--------------|---------------|-------------|
| `ANVIL_ARCH_S370` | HLASM | IBM S/370 |
| `ANVIL_ARCH_S370_XA` | HLASM | IBM S/370-XA |
| `ANVIL_ARCH_S390` | HLASM | IBM S/390 |
| `ANVIL_ARCH_ZARCH` | HLASM | z/Architecture |
| `ANVIL_ARCH_X86` | AT&T | x86 32-bit |
| `ANVIL_ARCH_X86_64` | AT&T | x86-64 |
| `ANVIL_ARCH_PPC32` | GAS | PowerPC 32-bit |
| `ANVIL_ARCH_PPC64` | GAS | PowerPC 64-bit |
| `ANVIL_ARCH_ARM64` | GAS | ARM64 (Linux) |
| `ANVIL_ARCH_ARM64` + Darwin ABI | GAS | ARM64 (Apple Silicon/macOS) |

## Code Generator API

### Data Structures

```c
typedef struct mcc_codegen {
    mcc_context_t *mcc_ctx;
    anvil_ctx_t *anvil_ctx;
    anvil_module_t *module;
    
    /* Symbol tables */
    mcc_symtab_t *symtab;
    mcc_type_context_t *types;
    
    /* Current function context */
    anvil_func_t *current_func;
    anvil_block_t *current_block;
    
    /* Local variable mapping (name -> anvil_value_t*) */
    struct {
        const char *name;
        anvil_value_t *value;
    } *locals;
    size_t num_locals;
    size_t cap_locals;
    
    /* Function table for external references */
    struct {
        mcc_symbol_t *sym;
        anvil_func_t *func;
    } *func_table;
    size_t num_funcs;
    size_t cap_funcs;
    
    /* Control flow for break/continue */
    anvil_block_t *break_target;
    anvil_block_t *continue_target;
    
    /* Label management */
    struct {
        const char *name;
        anvil_block_t *block;
    } *labels;
    size_t num_labels;
    size_t cap_labels;
    
    /* Target and optimization */
    anvil_arch_t target_arch;
    int opt_level;
} mcc_codegen_t;
```

### Functions

```c
/* Create and destroy */
mcc_codegen_t *mcc_codegen_create(mcc_context_t *ctx, 
                                   mcc_symtab_t *symtab,
                                   mcc_type_context_t *types);
void mcc_codegen_destroy(mcc_codegen_t *cg);

/* Configuration */
void mcc_codegen_set_target(mcc_codegen_t *cg, anvil_arch_t arch);
void mcc_codegen_set_opt_level(mcc_codegen_t *cg, int level);

/* Single-file code generation */
bool mcc_codegen_generate(mcc_codegen_t *cg, mcc_ast_node_t *ast);

/* Multi-file code generation */
bool mcc_codegen_add_ast(mcc_codegen_t *cg, mcc_ast_node_t *ast);
bool mcc_codegen_finalize(mcc_codegen_t *cg);

/* Get output */
char *mcc_codegen_get_output(mcc_codegen_t *cg, size_t *len);
```

## Type Mapping

C types are mapped to ANVIL types:

| C Type | ANVIL Type | Size |
|--------|------------|------|
| `char` | `anvil_type_i8` | 1 byte |
| `short` | `anvil_type_i16` | 2 bytes |
| `int` | `anvil_type_i32` | 4 bytes |
| `long` | `anvil_type_i32` or `i64` | 4/8 bytes |
| `float` | `anvil_type_f32` | 4 bytes |
| `double` | `anvil_type_f64` | 8 bytes |
| `void` | `anvil_type_void` | 0 bytes |
| `T*` | `anvil_type_ptr` | 4/8 bytes |
| `struct S` | `anvil_type_struct` | Sum of fields |

```c
/* In src/codegen/codegen_type.c */
anvil_type_t *codegen_type(mcc_codegen_t *cg, mcc_type_t *type)
{
    if (!type) return anvil_type_i32(cg->anvil_ctx);
    
    switch (type->kind) {
        case TYPE_VOID:   return anvil_type_void(cg->anvil_ctx);
        case TYPE_CHAR:   return anvil_type_i8(cg->anvil_ctx);
        case TYPE_SHORT:  return anvil_type_i16(cg->anvil_ctx);
        case TYPE_INT:    return anvil_type_i32(cg->anvil_ctx);
        case TYPE_LONG:   return anvil_type_i32(cg->anvil_ctx);
        case TYPE_FLOAT:  return anvil_type_f32(cg->anvil_ctx);
        case TYPE_DOUBLE: return anvil_type_f64(cg->anvil_ctx);
        case TYPE_POINTER:
            return anvil_type_ptr(cg->anvil_ctx, 
                                  codegen_type(cg, type->data.pointer.pointee));
        case TYPE_STRUCT:
        case TYPE_UNION: {
            /* Build struct type from fields */
            int num_fields = type->data.record.num_fields;
            anvil_type_t **field_types = mcc_alloc(cg->mcc_ctx,
                num_fields * sizeof(anvil_type_t*));
            int i = 0;
            for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next, i++) {
                field_types[i] = codegen_type(cg, f->type);
            }
            return anvil_type_struct(cg->anvil_ctx, NULL, field_types, num_fields);
        }
        case TYPE_FUNCTION: {
            anvil_type_t *ret_type = codegen_type(cg, type->data.function.return_type);
            /* ... build param types ... */
            return anvil_type_func(cg->anvil_ctx, ret_type, param_types, num_params,
                                   type->data.function.is_variadic);
        }
        default:
            return anvil_type_i32(cg->anvil_ctx);
    }
}
```

## Expression Code Generation

Expressions are generated recursively:

```c
/* In src/codegen/codegen_expr.c */
anvil_value_t *codegen_expr(mcc_codegen_t *cg, mcc_ast_node_t *expr)
{
    switch (expr->kind) {
        case AST_INT_LIT:
            return anvil_const_i32(cg->anvil_ctx, expr->data.int_lit.value);
            
        case AST_IDENT_EXPR: {
            anvil_value_t *ptr = codegen_find_local(cg, expr->data.ident_expr.name);
            anvil_type_t *type = codegen_type(cg, expr->type);
            return anvil_build_load(cg->anvil_ctx, type, ptr, "load");
        }
        
        case AST_BINARY_EXPR: {
            anvil_value_t *lhs = codegen_expr(cg, expr->data.binary_expr.lhs);
            anvil_value_t *rhs = codegen_expr(cg, expr->data.binary_expr.rhs);
            
            switch (expr->data.binary_expr.op) {
                case BINOP_ADD:
                    return anvil_build_add(cg->anvil_ctx, lhs, rhs, "add");
                case BINOP_SUB:
                    return anvil_build_sub(cg->anvil_ctx, lhs, rhs, "sub");
                case BINOP_MUL:
                    return anvil_build_mul(cg->anvil_ctx, lhs, rhs, "mul");
                /* ... */
            }
        }
        
        case AST_CALL_EXPR: {
            /* Generate arguments */
            anvil_value_t **args = /* ... */;
            anvil_func_t *func = codegen_get_or_declare_func(cg, /* ... */);
            return anvil_build_call(cg->anvil_ctx, func, args, num_args, "call");
        }
        
        case AST_MEMBER_EXPR: {
            /* Struct field access */
            anvil_value_t *obj = codegen_expr(cg, expr->data.member_expr.object);
            if (expr->data.member_expr.is_arrow) {
                /* obj is already a pointer */
            } else {
                /* Need address of obj */
            }
            int field_idx = find_field_index(/* ... */);
            return anvil_build_struct_gep(cg->anvil_ctx, struct_type, obj, 
                                          field_idx, "field");
        }
        
        /* ... */
    }
}
```

## Statement Code Generation

### Variable Declaration

```c
/* In src/codegen/codegen_stmt.c */
case AST_VAR_DECL: {
    anvil_type_t *type = codegen_type(cg, stmt->data.var_decl.var_type);
    anvil_value_t *alloca = anvil_build_alloca(cg->anvil_ctx, type, 
                                                stmt->data.var_decl.name);
    codegen_add_local(cg, stmt->data.var_decl.name, alloca);
    
    if (stmt->data.var_decl.init) {
        anvil_value_t *init = codegen_expr(cg, stmt->data.var_decl.init);
        anvil_build_store(cg->anvil_ctx, init, alloca);
    }
    break;
}
```

### If Statement

```c
/* In src/codegen/codegen_stmt.c */
case AST_IF_STMT: {
    anvil_value_t *cond = codegen_expr(cg, stmt->data.if_stmt.cond);
    
    anvil_block_t *then_bb = anvil_block_create(cg->anvil_ctx, "if.then");
    anvil_block_t *else_bb = stmt->data.if_stmt.else_stmt 
                            ? anvil_block_create(cg->anvil_ctx, "if.else")
                            : NULL;
    anvil_block_t *end_bb = anvil_block_create(cg->anvil_ctx, "if.end");
    
    anvil_build_cond_br(cg->anvil_ctx, cond, then_bb, 
                        else_bb ? else_bb : end_bb);
    
    /* Generate then block */
    anvil_func_append_block(cg->current_func, then_bb);
    anvil_set_insert_point(cg->anvil_ctx, then_bb);
    codegen_stmt(cg, stmt->data.if_stmt.then_stmt);
    anvil_build_br(cg->anvil_ctx, end_bb);
    
    /* Generate else block */
    if (else_bb) {
        anvil_func_append_block(cg->current_func, else_bb);
        anvil_set_insert_point(cg->anvil_ctx, else_bb);
        codegen_stmt(cg, stmt->data.if_stmt.else_stmt);
        anvil_build_br(cg->anvil_ctx, end_bb);
    }
    
    anvil_func_append_block(cg->current_func, end_bb);
    anvil_set_insert_point(cg->anvil_ctx, end_bb);
    break;
}
```

### While Statement

```c
/* In src/codegen/codegen_stmt.c */
case AST_WHILE_STMT: {
    anvil_block_t *cond_bb = anvil_block_create(cg->anvil_ctx, "while.cond");
    anvil_block_t *body_bb = anvil_block_create(cg->anvil_ctx, "while.body");
    anvil_block_t *end_bb = anvil_block_create(cg->anvil_ctx, "while.end");
    
    /* Save break/continue targets */
    anvil_block_t *old_break = cg->break_target;
    anvil_block_t *old_continue = cg->continue_target;
    cg->break_target = end_bb;
    cg->continue_target = cond_bb;
    
    anvil_build_br(cg->anvil_ctx, cond_bb);
    
    /* Condition */
    anvil_func_append_block(cg->current_func, cond_bb);
    anvil_set_insert_point(cg->anvil_ctx, cond_bb);
    anvil_value_t *cond = mcc_codegen_expr(cg, stmt->data.while_stmt.cond);
    anvil_build_cond_br(cg->anvil_ctx, cond, body_bb, end_bb);
    
    /* Body */
    anvil_func_append_block(cg->current_func, body_bb);
    anvil_set_insert_point(cg->anvil_ctx, body_bb);
    codegen_stmt(cg, stmt->data.while_stmt.body);
    anvil_build_br(cg->anvil_ctx, cond_bb);
    
    /* End */
    anvil_func_append_block(cg->current_func, end_bb);
    anvil_set_insert_point(cg->anvil_ctx, end_bb);
    
    /* Restore break/continue targets */
    cg->break_target = old_break;
    cg->continue_target = old_continue;
    break;
}
```

### Function Generation

```c
static void mcc_codegen_func(mcc_codegen_t *cg, mcc_ast_node_t *func_decl)
{
    /* Create function type */
    anvil_type_t *ret_type = mcc_codegen_type(cg, func_decl->data.func_decl.return_type);
    anvil_type_t **param_types = /* ... */;
    anvil_type_t *func_type = anvil_type_func(cg->anvil_ctx, ret_type, 
                                               param_types, num_params, false);
    
    /* Create function */
    anvil_func_t *func = anvil_func_create(cg->module, 
                                            func_decl->data.func_decl.name,
                                            func_type, ANVIL_LINK_EXTERNAL);
    cg->current_func = func;
    
    /* Reset locals */
    cg->num_locals = 0;
    
    /* Create entry block */
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(cg->anvil_ctx, entry);
    
    /* Allocate and store parameters */
    for (int i = 0; i < num_params; i++) {
        anvil_value_t *param = anvil_func_get_param(func, i);
        anvil_type_t *type = param_types[i];
        anvil_value_t *alloca = anvil_build_alloca(cg->anvil_ctx, type, 
                                                    param_names[i]);
        anvil_build_store(cg->anvil_ctx, param, alloca);
        add_local(cg, param_names[i], alloca);
    }
    
    /* Generate function body */
    codegen_stmt(cg, func_decl->data.func_decl.body);
    
    /* Add implicit return if needed */
    if (!anvil_block_has_terminator(cg->current_block)) {
        if (ret_type == anvil_type_void(cg->anvil_ctx)) {
            anvil_build_ret_void(cg->anvil_ctx);
        } else {
            anvil_build_ret(cg->anvil_ctx, anvil_const_i32(cg->anvil_ctx, 0));
        }
    }
}
```

## Local Variable Management

Local variables are tracked by name:

```c
static anvil_value_t *find_local(mcc_codegen_t *cg, const char *name)
{
    for (size_t i = 0; i < cg->num_locals; i++) {
        if (strcmp(cg->locals[i].name, name) == 0) {
            return cg->locals[i].value;
        }
    }
    return NULL;
}

static void add_local(mcc_codegen_t *cg, const char *name, anvil_value_t *value)
{
    if (cg->num_locals >= cg->cap_locals) {
        /* Grow array */
    }
    cg->locals[cg->num_locals].name = name;
    cg->locals[cg->num_locals].value = value;
    cg->num_locals++;
}
```

## External Function Handling

External functions (like `printf`) are declared on first use:

```c
static anvil_func_t *get_or_declare_func(mcc_codegen_t *cg, mcc_symbol_t *sym)
{
    /* Check if already declared */
    for (size_t i = 0; i < cg->num_funcs; i++) {
        if (cg->func_table[i].sym == sym) {
            return cg->func_table[i].func;
        }
    }
    
    /* Declare new function */
    anvil_type_t *func_type = mcc_codegen_type(cg, sym->type);
    anvil_func_t *func = anvil_func_create(cg->module, sym->name,
                                            func_type, ANVIL_LINK_EXTERNAL);
    
    /* Add to table */
    /* ... */
    
    return func;
}
```

## Generated Assembly Example

For this C code:

```c
int add(int a, int b) {
    return a + b;
}
```

MCC generates (for S/370):

```asm
ADD      DS    0H
         STM   R14,R12,12(R13)    Save caller's registers
         LR    R12,R15            Copy entry point to base reg
         USING ADD,R12            Establish addressability
         LR    R11,R1             Save parameter list pointer
*        Set up save area chain
         LA    R2,72(,R13)
         ST    R13,4(,R2)
         ST    R2,8(,R13)
         LR    R13,R2
*
ADD$ENTRY DS    0H
         L     R2,0(,R11)         Load addr of param 0
         L     R2,0(,R2)          Load param value
         ST    R2,88(,R13)        Store to stack slot
         L     R2,4(,R11)         Load addr of param 1
         L     R2,0(,R2)          Load param value
         ST    R2,92(,R13)        Store to stack slot
         L     R2,88(,R13)        Load a
         L     R3,92(,R13)        Load b
         AR    R2,R3              Add registers
         LR    R15,R2             Result in R15
*        Function epilogue
         L     R13,4(,R13)
         L     R14,12(,R13)
         LM    R0,R12,20(,R13)
         BR    R14
         DROP  R12
```

## Multi-File Compilation

MCC supports compiling multiple source files into a single output. This is useful for projects with separate compilation units.

### Usage

```bash
# Compile multiple files into one output
./mcc -o output.s file1.c file2.c file3.c

# With verbose output
./mcc -v -arch=x86_64 -o output.s main.c utils.c math.c
```

### How It Works

1. **Parsing Phase**: Each file is preprocessed and parsed independently, producing separate ASTs
2. **Semantic Analysis**: All ASTs are analyzed with a shared symbol table, allowing cross-file references
3. **Code Generation**: All ASTs are added to a single ANVIL module using `mcc_codegen_add_ast()`
4. **Finalization**: `mcc_codegen_finalize()` is called to complete code generation
5. **Output**: A single assembly file containing all functions and globals

### API for Multi-File

```c
/* Create codegen with shared symbol table */
mcc_codegen_t *cg = mcc_codegen_create(ctx, shared_symtab, shared_types);
mcc_codegen_set_target(cg, arch);

/* Add each file's AST */
for (int i = 0; i < num_files; i++) {
    mcc_codegen_add_ast(cg, asts[i]);
}

/* Finalize and get output */
mcc_codegen_finalize(cg);
char *output = mcc_codegen_get_output(cg, &len);
```

### Cross-File References

Functions declared in one file can be called from another:

**math.c:**
```c
int add(int a, int b) {
    return a + b;
}
```

**main.c:**
```c
int add(int a, int b);  /* Declaration */

int main(void) {
    return add(1, 2);   /* Call function from math.c */
}
```

## Optimization

MCC supports optimization levels:

- **-O0**: No optimization (default)
- **-O1**: Basic optimizations
- **-O2**: Standard optimizations
- **-O3**: Aggressive optimizations

Optimizations are performed by ANVIL on the IR before code generation.

## Recent Fixes

### Long Long Literals

Integer literals with `LL` or `ULL` suffix now use `anvil_const_i64`:

```c
/* In codegen_expr.c - codegen_expr() */
case AST_INT_LIT: {
    switch (expr->data.int_lit.suffix) {
        case INT_SUFFIX_LL:
        case INT_SUFFIX_ULL:
            return anvil_const_i64(cg->anvil_ctx, (int64_t)expr->data.int_lit.value);
        case INT_SUFFIX_L:
        case INT_SUFFIX_UL:
            return anvil_const_i64(cg->anvil_ctx, (int64_t)expr->data.int_lit.value);
        default:
            return anvil_const_i32(cg->anvil_ctx, (int32_t)expr->data.int_lit.value);
    }
}
```

### C99 For Loop Declarations

For loops with declarations now handle `init_decl`:

```c
/* In codegen_stmt.c - codegen_for_stmt() */
if (stmt->data.for_stmt.init_decl) {
    codegen_stmt(cg, stmt->data.for_stmt.init_decl);
} else if (stmt->data.for_stmt.init) {
    codegen_expr(cg, stmt->data.for_stmt.init);
}
```

### C99 `__func__` Support

The `__func__` predefined identifier generates a string literal with the function name:

```c
/* In codegen_expr.c - codegen_expr() */
if (expr->data.ident_expr.is_func_name) {
    const char *func_name = cg->current_func_name ? cg->current_func_name : "";
    return codegen_get_string_literal(cg, func_name);
}
```

The `current_func_name` is set when generating a function:

```c
/* In codegen_decl.c - codegen_func() */
cg->current_func_name = func->data.func_decl.name;
```
