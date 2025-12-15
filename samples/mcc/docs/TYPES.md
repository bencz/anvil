# MCC Type System Documentation

This document describes the type system component of MCC.

## Overview

The type system represents all C89 types and provides operations for type checking, conversions, and size/alignment calculations.

**Note:** `typedef` is not a type kind - it creates an alias to an existing type. Typedef names are tracked by the parser in a separate registry (`mcc_typedef_entry_t`) and resolved during parsing.

## Type Kinds

```c
typedef enum {
    TYPE_VOID,      /* void */
    TYPE_CHAR,      /* char */
    TYPE_SHORT,     /* short */
    TYPE_INT,       /* int */
    TYPE_LONG,      /* long */
    TYPE_FLOAT,     /* float */
    TYPE_DOUBLE,    /* double */
    TYPE_POINTER,   /* T* */
    TYPE_ARRAY,     /* T[N] */
    TYPE_FUNCTION,  /* T(params) */
    TYPE_STRUCT,    /* struct { ... } */
    TYPE_UNION,     /* union { ... } */
    TYPE_ENUM       /* enum { ... } */
} mcc_type_kind_t;
```

## Type Qualifiers

```c
#define QUAL_NONE     0
#define QUAL_CONST    1
#define QUAL_VOLATILE 2
```

## Type Structure

```c
typedef struct mcc_type {
    mcc_type_kind_t kind;
    int qualifiers;          /* QUAL_CONST | QUAL_VOLATILE */
    bool is_unsigned;        /* For integer types */
    int size;                /* Size in bytes */
    int align;               /* Alignment in bytes */
    
    union {
        /* Pointer type */
        struct {
            struct mcc_type *pointee;
        } pointer;
        
        /* Array type */
        struct {
            struct mcc_type *element;
            size_t size;     /* Number of elements, 0 for [] */
        } array;
        
        /* Function type */
        struct {
            struct mcc_type *return_type;
            struct mcc_type **param_types;
            int num_params;
            bool is_variadic;
            bool is_oldstyle; /* K&R style */
        } function;
        
        /* Struct/Union type */
        struct {
            const char *tag;
            mcc_struct_field_t *fields;
            int num_fields;
        } record;
        
        /* Enum type */
        struct {
            const char *tag;
            mcc_enum_const_t *constants;
            int num_constants;
        } enumeration;
    } data;
} mcc_type_t;
```

## Struct Fields

```c
typedef struct mcc_struct_field {
    const char *name;
    mcc_type_t *type;
    int offset;              /* Byte offset in struct */
    int bit_offset;          /* For bit fields */
    int bit_width;           /* For bit fields, 0 otherwise */
    struct mcc_struct_field *next;
} mcc_struct_field_t;
```

## Type Context

The type context manages type creation and caching:

```c
typedef struct mcc_type_context {
    mcc_context_t *ctx;
    
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
    mcc_type_t *type_float;
    mcc_type_t *type_double;
    mcc_type_t *type_ldouble;
    
    /* Pointer type cache */
    /* ... */
} mcc_type_context_t;
```

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
mcc_type_t *mcc_type_float(mcc_type_context_t *types);
mcc_type_t *mcc_type_double(mcc_type_context_t *types);
```

### Derived Types

```c
mcc_type_t *mcc_type_pointer(mcc_type_context_t *types, mcc_type_t *pointee);
mcc_type_t *mcc_type_array(mcc_type_context_t *types, mcc_type_t *element, size_t size);
mcc_type_t *mcc_type_function(mcc_type_context_t *types, mcc_type_t *ret,
                               mcc_type_t **params, int num_params, bool variadic);
mcc_type_t *mcc_type_struct(mcc_type_context_t *types, const char *tag);
mcc_type_t *mcc_type_union(mcc_type_context_t *types, const char *tag);
mcc_type_t *mcc_type_enum(mcc_type_context_t *types, const char *tag);
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
bool mcc_type_is_record(mcc_type_t *type);  /* struct or union */
bool mcc_type_is_complete(mcc_type_t *type);
bool mcc_type_is_compatible(mcc_type_t *a, mcc_type_t *b);
```

### Type Properties

```c
int mcc_type_size(mcc_type_t *type);
int mcc_type_align(mcc_type_t *type);
mcc_struct_field_t *mcc_type_find_field(mcc_type_t *type, const char *name);
```

### Type Conversions

```c
mcc_type_t *mcc_type_promote(mcc_type_context_t *types, mcc_type_t *type);
mcc_type_t *mcc_type_common(mcc_type_context_t *types, mcc_type_t *a, mcc_type_t *b);
```

## Type Sizes and Alignment

Default sizes (may vary by target):

| Type | Size | Alignment |
|------|------|-----------|
| `char` | 1 | 1 |
| `short` | 2 | 2 |
| `int` | 4 | 4 |
| `long` | 4 | 4 |
| `float` | 4 | 4 |
| `double` | 8 | 8 |
| `pointer` | 4 | 4 |

## Integer Promotions

Small integer types are promoted to `int` in expressions:

```c
mcc_type_t *mcc_type_promote(mcc_type_context_t *types, mcc_type_t *type)
{
    if (!mcc_type_is_integer(type)) return type;
    
    /* char, short, bit-fields promote to int */
    if (type->kind == TYPE_CHAR || type->kind == TYPE_SHORT) {
        return mcc_type_int(types);
    }
    
    return type;
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

Structs are laid out with proper alignment:

```c
void mcc_type_layout_struct(mcc_type_t *type)
{
    int offset = 0;
    int max_align = 1;
    
    for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next) {
        int field_align = mcc_type_align(f->type);
        int field_size = mcc_type_size(f->type);
        
        /* Align offset */
        offset = (offset + field_align - 1) & ~(field_align - 1);
        
        f->offset = offset;
        offset += field_size;
        
        if (field_align > max_align) {
            max_align = field_align;
        }
    }
    
    /* Pad struct to alignment */
    type->size = (offset + max_align - 1) & ~(max_align - 1);
    type->align = max_align;
}
```

## Union Layout

Unions have all fields at offset 0:

```c
void mcc_type_layout_union(mcc_type_t *type)
{
    int max_size = 0;
    int max_align = 1;
    
    for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next) {
        f->offset = 0;
        
        int field_size = mcc_type_size(f->type);
        int field_align = mcc_type_align(f->type);
        
        if (field_size > max_size) max_size = field_size;
        if (field_align > max_align) max_align = field_align;
    }
    
    type->size = (max_size + max_align - 1) & ~(max_align - 1);
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

/* Function: int(int, int) */
mcc_type_t *params[] = { int_type, int_type };
mcc_type_t *func_type = mcc_type_function(types, int_type, params, 2, false);

/* Check properties */
printf("int size: %d\n", mcc_type_size(int_type));        /* 4 */
printf("int* size: %d\n", mcc_type_size(int_ptr));        /* 4 */
printf("int[10] size: %d\n", mcc_type_size(int_array));   /* 40 */

mcc_type_context_destroy(types);
```
