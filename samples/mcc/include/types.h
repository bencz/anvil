/*
 * MCC - Micro C Compiler
 * C Type System
 */

#ifndef MCC_TYPES_H
#define MCC_TYPES_H

/* Type kinds */
typedef enum {
    TYPE_VOID,
    TYPE_CHAR,
    TYPE_SHORT,
    TYPE_INT,
    TYPE_LONG,
    TYPE_LONG_LONG,     /* C99 long long */
    TYPE_FLOAT,
    TYPE_DOUBLE,
    TYPE_LONG_DOUBLE,
    TYPE_BOOL,          /* C99 _Bool */
    TYPE_POINTER,
    TYPE_ARRAY,
    TYPE_FUNCTION,
    TYPE_STRUCT,
    TYPE_UNION,
    TYPE_ENUM,
    TYPE_TYPEDEF,       /* Reference to typedef'd type */
    
    TYPE_COUNT
} mcc_type_kind_t;

/* Type qualifiers (can be combined) */
typedef enum {
    QUAL_NONE     = 0,
    QUAL_CONST    = 1 << 0,
    QUAL_VOLATILE = 1 << 1,
    QUAL_RESTRICT = 1 << 2,     /* C99 restrict */
    QUAL_ATOMIC   = 1 << 3      /* C11 _Atomic */
} mcc_type_qual_t;

/* Storage class */
typedef enum {
    STORAGE_NONE,
    STORAGE_AUTO,
    STORAGE_REGISTER,
    STORAGE_STATIC,
    STORAGE_EXTERN,
    STORAGE_TYPEDEF
} mcc_storage_class_t;

/* Forward declarations */
typedef struct mcc_type mcc_type_t;
typedef struct mcc_struct_field mcc_struct_field_t;
typedef struct mcc_func_param mcc_func_param_t;

/* Struct/union field */
struct mcc_struct_field {
    const char *name;
    mcc_type_t *type;
    int offset;                 /* Byte offset (computed) */
    int bitfield_width;         /* 0 if not a bitfield */
    mcc_struct_field_t *next;
};

/* Enum constant */
typedef struct mcc_enum_const {
    const char *name;
    int64_t value;
    struct mcc_enum_const *next;
} mcc_enum_const_t;

/* Function parameter */
struct mcc_func_param {
    const char *name;           /* Can be NULL */
    mcc_type_t *type;
    mcc_func_param_t *next;
};

/* Type structure */
struct mcc_type {
    mcc_type_kind_t kind;
    mcc_type_qual_t qualifiers;
    bool is_unsigned;           /* For integer types */
    bool is_inline;             /* C99 inline function specifier */
    bool is_noreturn;           /* C11 _Noreturn function specifier */
    
    /* Size and alignment (computed) */
    size_t size;
    size_t align;
    
    union {
        /* Pointer type */
        struct {
            mcc_type_t *pointee;
        } pointer;
        
        /* Array type */
        struct {
            mcc_type_t *element;
            size_t length;      /* 0 for incomplete array */
            bool is_vla;        /* Variable length array (C99) */
            bool is_flexible;   /* Flexible array member (C99) */
        } array;
        
        /* Function type */
        struct {
            mcc_type_t *return_type;
            mcc_func_param_t *params;
            int num_params;
            bool is_variadic;
            bool is_oldstyle;   /* K&R style declaration */
        } function;
        
        /* Struct/union type */
        struct {
            const char *tag;    /* NULL for anonymous */
            mcc_struct_field_t *fields;
            int num_fields;
            bool is_complete;   /* Has definition? */
        } record;               /* Used for both struct and union */
        
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
    
    /* For type caching */
    struct mcc_type *next;
};

/* Type context for caching */
typedef struct mcc_type_context {
    mcc_context_t *ctx;
    
    /* Cached primitive types */
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
    
    /* Type hash table for deduplication */
    mcc_type_t **type_table;
    size_t type_table_size;
} mcc_type_context_t;

/* Type context management */
mcc_type_context_t *mcc_type_context_create(mcc_context_t *ctx);
void mcc_type_context_destroy(mcc_type_context_t *tctx);

/* Primitive type getters */
mcc_type_t *mcc_type_void(mcc_type_context_t *tctx);
mcc_type_t *mcc_type_char(mcc_type_context_t *tctx);
mcc_type_t *mcc_type_schar(mcc_type_context_t *tctx);
mcc_type_t *mcc_type_uchar(mcc_type_context_t *tctx);
mcc_type_t *mcc_type_short(mcc_type_context_t *tctx);
mcc_type_t *mcc_type_ushort(mcc_type_context_t *tctx);
mcc_type_t *mcc_type_int(mcc_type_context_t *tctx);
mcc_type_t *mcc_type_uint(mcc_type_context_t *tctx);
mcc_type_t *mcc_type_long(mcc_type_context_t *tctx);
mcc_type_t *mcc_type_ulong(mcc_type_context_t *tctx);
mcc_type_t *mcc_type_float(mcc_type_context_t *tctx);
mcc_type_t *mcc_type_double(mcc_type_context_t *tctx);
mcc_type_t *mcc_type_long_double(mcc_type_context_t *tctx);

/* Derived type constructors */
mcc_type_t *mcc_type_pointer(mcc_type_context_t *tctx, mcc_type_t *pointee);
mcc_type_t *mcc_type_array(mcc_type_context_t *tctx, mcc_type_t *element, size_t length);
mcc_type_t *mcc_type_incomplete_array(mcc_type_context_t *tctx, mcc_type_t *element);
mcc_type_t *mcc_type_function(mcc_type_context_t *tctx, mcc_type_t *return_type,
                               mcc_func_param_t *params, int num_params, bool variadic);
mcc_type_t *mcc_type_struct(mcc_type_context_t *tctx, const char *tag);
mcc_type_t *mcc_type_union(mcc_type_context_t *tctx, const char *tag);
mcc_type_t *mcc_type_enum(mcc_type_context_t *tctx, const char *tag);

/* Type completion */
void mcc_type_complete_struct(mcc_type_t *type, mcc_struct_field_t *fields, int num_fields);
void mcc_type_complete_union(mcc_type_t *type, mcc_struct_field_t *fields, int num_fields);
void mcc_type_complete_enum(mcc_type_t *type);

/* Type qualifiers */
mcc_type_t *mcc_type_qualified(mcc_type_context_t *tctx, mcc_type_t *type, mcc_type_qual_t quals);
mcc_type_t *mcc_type_unqualified(mcc_type_t *type);

/* Type queries */
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
bool mcc_type_is_record(mcc_type_t *type);  /* struct or union */
bool mcc_type_is_enum(mcc_type_t *type);
bool mcc_type_is_aggregate(mcc_type_t *type); /* array or record */
bool mcc_type_is_complete(mcc_type_t *type);
bool mcc_type_is_compatible(mcc_type_t *a, mcc_type_t *b);
bool mcc_type_is_same(mcc_type_t *a, mcc_type_t *b);

/* Type conversions */
mcc_type_t *mcc_type_promote(mcc_type_context_t *tctx, mcc_type_t *type);
mcc_type_t *mcc_type_common(mcc_type_context_t *tctx, mcc_type_t *a, mcc_type_t *b);
mcc_type_t *mcc_type_decay(mcc_type_context_t *tctx, mcc_type_t *type);

/* Type utilities */
const char *mcc_type_kind_name(mcc_type_kind_t kind);
char *mcc_type_to_string(mcc_type_t *type);
size_t mcc_type_sizeof(mcc_type_t *type);
size_t mcc_type_alignof(mcc_type_t *type);

/* Struct field lookup */
mcc_struct_field_t *mcc_type_find_field(mcc_type_t *type, const char *name);

#endif /* MCC_TYPES_H */
