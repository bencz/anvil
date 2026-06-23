# MCC Type System Documentation

This document describes the type system component of MCC.

## Overview

The type system represents all C types and provides operations for type checking, conversions, and size/alignment calculations. The type system supports C89 through C23 features.

**Note:** `typedef` is not a type kind - it creates an alias to an existing type. Typedef names are tracked by the parser in a separate registry (`mcc_typedef_entry_t`) and resolved during parsing.

## Type Kinds

```c
typedef enum {
    /* C89 types */
    TYPE_VOID,             /* void */
    TYPE_CHAR,             /* char */
    TYPE_SHORT,            /* short */
    TYPE_INT,              /* int */
    TYPE_LONG,             /* long */
    TYPE_LONG_LONG,        /* long long (C99) */
    TYPE_FLOAT,            /* float */
    TYPE_DOUBLE,           /* double */
    TYPE_LONG_DOUBLE,      /* long double */

    /* C99 types */
    TYPE_BOOL,             /* _Bool */
    TYPE_COMPLEX_FLOAT,    /* float _Complex */
    TYPE_COMPLEX_DOUBLE,   /* double _Complex */
    TYPE_COMPLEX_LDOUBLE,  /* long double _Complex */

    /* Derived types */
    TYPE_POINTER,          /* T* */
    TYPE_ARRAY,            /* T[N] */
    TYPE_FUNCTION,         /* T(params) */
    TYPE_STRUCT,           /* struct { ... } */
    TYPE_UNION,            /* union { ... } */
    TYPE_ENUM,             /* enum { ... } */

    /* Typedef reference (resolved to its underlying type) */
    TYPE_TYPEDEF,          /* reference to a typedef'd type */

    TYPE_COUNT
} mcc_type_kind_t;
```

The `_Complex` kinds are defined and constructed (`mcc_type_complex_float`/`_double`/`_ldouble`),
sized as twice their component type. `TYPE_TYPEDEF` is the type-system representation of a typedef
reference; it carries the name and the underlying type.

## Type Qualifiers

```c
typedef enum {
    QUAL_NONE     = 0,
    QUAL_CONST    = 1 << 0,   /* C89: const */
    QUAL_VOLATILE = 1 << 1,   /* C89: volatile */
    QUAL_RESTRICT = 1 << 2,   /* C99: restrict */
    QUAL_ATOMIC   = 1 << 3    /* C11: _Atomic */
} mcc_type_qual_t;
```

## Type Structure

```c
struct mcc_type {
    mcc_type_kind_t kind;
    mcc_type_qual_t qualifiers;  /* QUAL_CONST | QUAL_VOLATILE | QUAL_RESTRICT | QUAL_ATOMIC */
    bool is_unsigned;            /* For integer types */
    bool is_inline;              /* C99: inline function specifier */
    bool is_noreturn;            /* C11: _Noreturn function specifier */
    size_t size;                 /* Size in bytes (computed) */
    size_t align;                /* Alignment in bytes (computed) */

    union {
        /* Pointer type */
        struct {
            mcc_type_t *pointee;
        } pointer;

        /* Array type */
        struct {
            mcc_type_t *element;
            size_t length;       /* Number of elements, 0 for incomplete [] */
            bool is_vla;         /* C99: Variable Length Array */
            bool is_flexible;    /* C99: Flexible array member */
            struct mcc_ast_node *length_expr; /* VLA runtime count; NULL otherwise */
        } array;

        /* Function type */
        struct {
            mcc_type_t *return_type;
            mcc_func_param_t *params;   /* Linked list of parameters */
            int num_params;
            bool is_variadic;
            bool is_oldstyle;    /* K&R style declaration */
        } function;

        /* Struct/union type (record is used for both) */
        struct {
            const char *tag;     /* NULL for anonymous */
            mcc_struct_field_t *fields;
            int num_fields;
            bool is_complete;    /* Has a definition? */
        } record;

        /* Enum type */
        struct {
            const char *tag;
            mcc_enum_const_t *constants;
            int num_constants;
            bool is_complete;
        } enumeration;

        /* Typedef reference */
        struct {
            const char *name;
            mcc_type_t *underlying;
        } typedef_ref;
    } data;

    struct mcc_type *next;       /* For the dedup type cache */
    void *anvil_cached;          /* Codegen-side cache of the derived Anvil type */
};
```

Note the function type stores its parameters as a linked list of `mcc_func_param_t`
(field `params`), not an array of type pointers.

## Struct Fields

```c
typedef struct mcc_struct_field {
    const char *name;
    mcc_type_t *type;
    int offset;              /* Byte offset in struct */
    int bitfield_width;      /* 0 if not a bitfield */
    struct mcc_struct_field *next;
} mcc_struct_field_t;
```

## Enum Constants

```c
typedef struct mcc_enum_const {
    const char *name;
    int64_t value;
    struct mcc_enum_const *next;
} mcc_enum_const_t;
```

## Function Parameters

```c
typedef struct mcc_func_param {
    const char *name;           /* Can be NULL for abstract declarators */
    mcc_type_t *type;
    struct mcc_func_param *next;
} mcc_func_param_t;
```

## Type Context

The type context manages type creation and caching. It queries ANVIL for architecture-specific type sizes:

```c
typedef struct mcc_type_context {
    mcc_context_t *ctx;
    
    /* Architecture-specific sizes (from ANVIL) */
    int ptr_size;               /* Pointer size from anvil_arch_get_info() */
    
    /* Cached basic types */
    mcc_type_t *type_void;
    mcc_type_t *type_char;
    mcc_type_t *type_schar;
    mcc_type_t *type_uchar;
    mcc_type_t *type_short;
    mcc_type_t *type_ushort;
    mcc_type_t *type_int;
    mcc_type_t *type_uint;
    mcc_type_t *type_long;
    mcc_type_t *type_ulong;
    mcc_type_t *type_llong;     /* C99: long long */
    mcc_type_t *type_ullong;    /* C99: unsigned long long */
    mcc_type_t *type_float;
    mcc_type_t *type_double;
    mcc_type_t *type_ldouble;
    mcc_type_t *type_cfloat;    /* C99: float _Complex */
    mcc_type_t *type_cdouble;   /* C99: double _Complex */
    mcc_type_t *type_cldouble;  /* C99: long double _Complex */

    /* Type hash table for deduplication */
    mcc_type_t **type_table;
    size_t type_table_size;
} mcc_type_context_t;
```

During initialization, `mcc_type_context_create()` uses `mcc_arch_to_anvil()` and `anvil_arch_get_info()` to obtain the target architecture's `ptr_size` and `word_size`, ensuring correct type sizes for cross-compilation.

## Type API

### Type Context

```c
mcc_type_context_t *mcc_type_context_create(mcc_context_t *ctx);
void mcc_type_context_destroy(mcc_type_context_t *types);
```

### Basic Types

```c
mcc_type_t *mcc_type_void(mcc_type_context_t *types);
mcc_type_t *mcc_type_char(mcc_type_context_t *types);
mcc_type_t *mcc_type_schar(mcc_type_context_t *types);
mcc_type_t *mcc_type_uchar(mcc_type_context_t *types);
mcc_type_t *mcc_type_short(mcc_type_context_t *types);
mcc_type_t *mcc_type_ushort(mcc_type_context_t *types);
mcc_type_t *mcc_type_int(mcc_type_context_t *types);
mcc_type_t *mcc_type_uint(mcc_type_context_t *types);
mcc_type_t *mcc_type_long(mcc_type_context_t *types);
mcc_type_t *mcc_type_ulong(mcc_type_context_t *types);
mcc_type_t *mcc_type_llong(mcc_type_context_t *types);   /* C99: long long */
mcc_type_t *mcc_type_ullong(mcc_type_context_t *types);  /* C99: unsigned long long */
mcc_type_t *mcc_type_float(mcc_type_context_t *types);
mcc_type_t *mcc_type_double(mcc_type_context_t *types);
mcc_type_t *mcc_type_long_double(mcc_type_context_t *types);
mcc_type_t *mcc_type_complex_float(mcc_type_context_t *types);   /* C99 */
mcc_type_t *mcc_type_complex_double(mcc_type_context_t *types);  /* C99 */
mcc_type_t *mcc_type_complex_ldouble(mcc_type_context_t *types); /* C99 */
```

### Derived Types

```c
mcc_type_t *mcc_type_pointer(mcc_type_context_t *types, mcc_type_t *pointee);
mcc_type_t *mcc_type_array(mcc_type_context_t *types, mcc_type_t *element, size_t length);
mcc_type_t *mcc_type_incomplete_array(mcc_type_context_t *types, mcc_type_t *element);
mcc_type_t *mcc_type_function(mcc_type_context_t *types, mcc_type_t *ret,
                               mcc_func_param_t *params, int num_params, bool variadic);
mcc_type_t *mcc_type_struct(mcc_type_context_t *types, const char *tag);
mcc_type_t *mcc_type_union(mcc_type_context_t *types, const char *tag);
mcc_type_t *mcc_type_enum(mcc_type_context_t *types, const char *tag);

/* Completion (called once a struct/union/enum body is parsed) */
void mcc_type_complete_struct(mcc_type_t *type, mcc_struct_field_t *fields, int num_fields);
void mcc_type_complete_union(mcc_type_t *type, mcc_struct_field_t *fields, int num_fields);
void mcc_type_complete_enum(mcc_type_t *type);

/* Qualifiers */
mcc_type_t *mcc_type_qualified(mcc_type_context_t *types, mcc_type_t *type, mcc_type_qual_t quals);
mcc_type_t *mcc_type_unqualified(mcc_type_t *type);
```

### Type Predicates

```c
bool mcc_type_is_void(mcc_type_t *type);
bool mcc_type_is_integer(mcc_type_t *type);
bool mcc_type_is_floating(mcc_type_t *type);
bool mcc_type_is_arithmetic(mcc_type_t *type);
bool mcc_type_is_scalar(mcc_type_t *type);
bool mcc_type_is_pointer(mcc_type_t *type);
bool mcc_type_is_array(mcc_type_t *type);
bool mcc_type_is_function(mcc_type_t *type);
bool mcc_type_is_struct(mcc_type_t *type);
bool mcc_type_is_union(mcc_type_t *type);
bool mcc_type_is_record(mcc_type_t *type);     /* struct or union */
bool mcc_type_is_enum(mcc_type_t *type);
bool mcc_type_is_aggregate(mcc_type_t *type);  /* array or record */
bool mcc_type_is_complete(mcc_type_t *type);
bool mcc_type_is_compatible(mcc_type_t *a, mcc_type_t *b);
bool mcc_type_is_same(mcc_type_t *a, mcc_type_t *b);
```

### Type Properties

```c
size_t mcc_type_sizeof(mcc_type_t *type);
size_t mcc_type_alignof(mcc_type_t *type);
const char *mcc_type_kind_name(mcc_type_kind_t kind);
char *mcc_type_to_string(mcc_type_t *type);
mcc_struct_field_t *mcc_type_find_field(mcc_type_t *type, const char *name);
```

(Size and alignment are also stored directly in the `size`/`align` fields of `mcc_type_t`,
which are filled in when the type is constructed/completed.)

### Type Conversions

```c
mcc_type_t *mcc_type_promote(mcc_type_context_t *types, mcc_type_t *type);
mcc_type_t *mcc_type_common(mcc_type_context_t *types, mcc_type_t *a, mcc_type_t *b);
mcc_type_t *mcc_type_decay(mcc_type_context_t *types, mcc_type_t *type);  /* array/function decay */
```

## Type Sizes and Alignment

Type sizes are determined by the target architecture using ANVIL's `anvil_arch_get_info()`. MCC supports multiple data models:

### Data Models

| Model | `int` | `long` | `pointer` | Architectures |
|-------|-------|--------|-----------|---------------|
| **ILP32** | 4 | 4 | 4 | x86, S/370 (24-bit), S/370-XA (31-bit), S/390 (31-bit), PPC32 |
| **LP64** | 4 | 8 | 8 | x86_64, z/Architecture (64-bit), PPC64, PPC64LE, ARM64 |
| **Darwin LP64** | 4 | 8 | 8 | ARM64-macOS (long double = 8) |
| **LLP64** | 4 | 4 | 8 | Windows x64 (requires `ANVIL_ABI_WIN64`) |

### IBM Mainframe Addressing Modes

IBM mainframes have unique addressing modes that don't directly map to pointer sizes:

| Architecture | Address Bits | Pointer Size | Data Model |
|--------------|--------------|--------------|------------|
| S/370 | 24-bit | 4 bytes | ILP32 |
| S/370-XA | 31-bit | 4 bytes | ILP32 |
| S/390 | 31-bit | 4 bytes | ILP32 |
| z/Architecture | 64-bit | 8 bytes | LP64 |

Note: Even with 24-bit or 31-bit addressing, pointers are stored in 32-bit registers/memory on S/370 and S/390.

### Default Type Sizes

| Type | ILP32 | LP64 | Standard |
|------|-------|------|----------|
| `char` | 1 | 1 | C89 |
| `short` | 2 | 2 | C89 |
| `int` | 4 | 4 | C89 |
| `long` | 4 | **8** | C89 |
| `long long` | 8 | 8 | C99 |
| `_Bool` | 1 | 1 | C99 |
| `float` | 4 | 4 | C89 |
| `double` | 8 | 8 | C89 |
| `long double` | 8 | 16* | C89 |
| `pointer` | 4 | 8 | C89 |

*Darwin LP64 uses 8 bytes for `long double`.

## Integer Promotions

Small integer types are promoted to `int` in expressions:

```c
mcc_type_t *mcc_type_promote(mcc_type_context_t *tctx, mcc_type_t *type)
{
    /* Integer promotion: char, short -> int (unsigned variants -> unsigned int); enum -> int */
    switch (type->kind) {
        case TYPE_CHAR:
        case TYPE_SHORT:
            return type->is_unsigned ? tctx->type_uint : tctx->type_int;
        case TYPE_ENUM:
            return tctx->type_int;
        default:
            return type;
    }
}
```

## Usual Arithmetic Conversions

When two operands have different types:

```c
mcc_type_t *mcc_type_common(mcc_type_context_t *types, mcc_type_t *a, mcc_type_t *b)
{
    /* If either is long double, result is long double */
    /* If either is double, result is double */
    /* If either is float, result is float */
    /* Otherwise, integer promotions then:
       - If both same signedness, use larger
       - If unsigned has >= rank, use unsigned
       - If signed can represent all unsigned values, use signed
       - Otherwise, use unsigned version of signed type */
}
```

## Struct Layout

Struct layout is computed by `mcc_type_complete_struct()` when the body is parsed.
Fields are laid out in order with proper alignment, and the final size is padded to
the struct's alignment:

```c
void mcc_type_complete_struct(mcc_type_t *type, mcc_struct_field_t *fields, int num_fields)
{
    size_t offset = 0;
    size_t max_align = 1;

    for (mcc_struct_field_t *f = fields; f; f = f->next) {
        size_t align = f->type->align;
        if (align > max_align) max_align = align;

        /* Align this field's offset */
        offset = (offset + align - 1) & ~(align - 1);
        f->offset = (int)offset;
        offset += f->type->size;
    }

    /* Pad struct size to its alignment */
    type->size  = (offset + max_align - 1) & ~(max_align - 1);
    type->align = max_align;
    /* ...also records fields, num_fields, and marks the record complete... */
}
```

## Union Layout

Unions are laid out by `mcc_type_complete_union()` with all fields at offset 0:

```c
void mcc_type_complete_union(mcc_type_t *type, mcc_struct_field_t *fields, int num_fields)
{
    size_t max_size = 0;
    size_t max_align = 1;

    for (mcc_struct_field_t *f = fields; f; f = f->next) {
        f->offset = 0;
        if (f->type->size  > max_size)  max_size  = f->type->size;
        if (f->type->align > max_align) max_align = f->type->align;
    }

    type->size  = (max_size + max_align - 1) & ~(max_align - 1);
    type->align = max_align;
}
```

## Type Compatibility

Two types are compatible if:
- They are the same type
- Both are pointers to compatible types
- Both are arrays of compatible element types
- Both are functions with compatible return types and parameters
- Both are the same struct/union/enum (by tag)

```c
bool mcc_type_is_compatible(mcc_type_t *a, mcc_type_t *b)
{
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    
    switch (a->kind) {
        case TYPE_POINTER:
            return mcc_type_is_compatible(a->data.pointer.pointee,
                                          b->data.pointer.pointee);
        case TYPE_ARRAY:
            return mcc_type_is_compatible(a->data.array.element,
                                          b->data.array.element);
        case TYPE_FUNCTION:
            /* Check return type and parameters */
            /* ... */
        case TYPE_STRUCT:
        case TYPE_UNION:
            return a->data.record.tag && b->data.record.tag &&
                   strcmp(a->data.record.tag, b->data.record.tag) == 0;
        default:
            return a->is_unsigned == b->is_unsigned;
    }
}
```

## Usage Example

```c
mcc_type_context_t *types = mcc_type_context_create(ctx);

/* Basic types */
mcc_type_t *int_type = mcc_type_int(types);
mcc_type_t *char_type = mcc_type_char(types);

/* Pointer to int */
mcc_type_t *int_ptr = mcc_type_pointer(types, int_type);

/* Array of 10 ints */
mcc_type_t *int_array = mcc_type_array(types, int_type, 10);

/* Function: int(void) — parameters are passed as an mcc_func_param_t list */
mcc_type_t *func_type = mcc_type_function(types, int_type, NULL, 0, false);

/* Check properties (returns size_t) */
printf("int size: %zu\n", mcc_type_sizeof(int_type));        /* 4 */
printf("int* size: %zu\n", mcc_type_sizeof(int_ptr));        /* 4 or 8, target-dependent */
printf("int[10] size: %zu\n", mcc_type_sizeof(int_array));   /* 40 */

mcc_type_context_destroy(types);
```

## Recent Fixes

### Integer Type Checking

`mcc_type_is_integer()` now includes `TYPE_LONG_LONG` and `TYPE_BOOL`:

```c
/* In types.c */
bool mcc_type_is_integer(mcc_type_t *type)
{
    switch (type->kind) {
        case TYPE_CHAR:
        case TYPE_SHORT:
        case TYPE_INT:
        case TYPE_LONG:
        case TYPE_LONG_LONG:  /* Added */
        case TYPE_BOOL:       /* Added */
        case TYPE_ENUM:
            return true;
        default:
            return false;
    }
}
```

This ensures proper type compatibility checking for C99 `long long` and C99/C23 `_Bool`/`bool` types.

### Anonymous Field Lookup

The `mcc_type_find_field()` function handles unnamed fields in two ways: anonymous
bitfield padding (e.g. `unsigned int : 0;`) is skipped, while anonymous struct/union
members (C11) are recursed into so that an inner member can be found through the outer
type:

```c
/* In types.c */
mcc_struct_field_t *mcc_type_find_field(mcc_type_t *type, const char *name)
{
    if (type->kind != TYPE_STRUCT && type->kind != TYPE_UNION) {
        return NULL;
    }

    for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next) {
        if (f->name) {
            if (strcmp(f->name, name) == 0) return f;
            continue;
        }
        /* Anonymous field: recurse into anonymous struct/union members so that
         * `o.x` finds `x` in `struct outer { struct { int x; }; }`.
         * Bitfield padding has a non-record type and the recursion bails out. */
        if (f->type && (f->type->kind == TYPE_STRUCT || f->type->kind == TYPE_UNION)) {
            mcc_struct_field_t *inner = mcc_type_find_field(f->type, name);
            if (inner) return inner;
        }
    }

    return NULL;
}
```
