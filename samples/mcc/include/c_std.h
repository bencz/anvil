/*
 * MCC - Micro C Compiler
 * C Language Standard Definitions
 * 
 * This file defines the C language standards (C89, C90, C99, etc.)
 * and their associated features, similar to how ANVIL handles CPU models.
 */

#ifndef MCC_C_STD_H
#define MCC_C_STD_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*
 * C Language Standard Versions
 * 
 * Note: C89 and C90 are essentially the same standard.
 * - C89: ANSI X3.159-1989 (American National Standard)
 * - C90: ISO/IEC 9899:1990 (International Standard, identical to C89)
 * - C99: ISO/IEC 9899:1999
 * - C11: ISO/IEC 9899:2011 (future support)
 * - C17: ISO/IEC 9899:2018 (future support)
 * - C23: ISO/IEC 9899:2024 (future support)
 */
typedef enum {
    MCC_STD_DEFAULT = 0,    /* Use compiler default (currently C89) */
    MCC_STD_C89,            /* ANSI C89 / ISO C90 */
    MCC_STD_C90,            /* Alias for C89 (ISO version) */
    MCC_STD_C99,            /* ISO C99 */
    MCC_STD_C11,            /* ISO C11 (future) */
    MCC_STD_C17,            /* ISO C17 (future) */
    MCC_STD_C23,            /* ISO C23 (future) */
    MCC_STD_GNU89,          /* GNU extensions to C89 */
    MCC_STD_GNU99,          /* GNU extensions to C99 */
    MCC_STD_GNU11,          /* GNU extensions to C11 (future) */
    MCC_STD_COUNT
} mcc_c_std_t;

/* ============================================================
 * Scalable Feature System
 * 
 * Uses an array of uint64_t to support unlimited features.
 * Currently sized for 256 features (4 x 64-bit words).
 * Can be expanded by increasing MCC_FEATURE_WORDS.
 * ============================================================ */

#define MCC_FEATURE_WORDS 4     /* 4 * 64 = 256 features */
#define MCC_FEATURE_BITS  64    /* Bits per word */

/*
 * Feature set structure - array of 64-bit words
 */
typedef struct mcc_c_features {
    uint64_t bits[MCC_FEATURE_WORDS];
} mcc_c_features_t;

/*
 * Feature ID - identifies a single feature
 * Range: 0 to (MCC_FEATURE_WORDS * 64 - 1)
 */
typedef enum {
    /* ============================================================
     * Word 0: C89/C90 Core Features (0-63)
     * ============================================================ */
    
    /* C89 Types (0-15) */
    MCC_FEAT_BASIC_TYPES = 0,       /* char, short, int, long, float, double */
    MCC_FEAT_POINTERS,              /* Pointer types */
    MCC_FEAT_ARRAYS,                /* Array types */
    MCC_FEAT_STRUCTS,               /* struct types */
    MCC_FEAT_UNIONS,                /* union types */
    MCC_FEAT_ENUMS,                 /* enum types */
    MCC_FEAT_TYPEDEF,               /* typedef declarations */
    MCC_FEAT_CONST,                 /* const qualifier */
    MCC_FEAT_VOLATILE,              /* volatile qualifier */
    MCC_FEAT_SIGNED,                /* signed keyword */
    MCC_FEAT_UNSIGNED,              /* unsigned keyword */
    MCC_FEAT_VOID,                  /* void type */
    MCC_FEAT_CHAR,                  /* char type */
    MCC_FEAT_SHORT,                 /* short type */
    MCC_FEAT_INT,                   /* int type */
    MCC_FEAT_LONG,                  /* long type */
    
    /* C89 Types continued (16-23) */
    MCC_FEAT_FLOAT,                 /* float type */
    MCC_FEAT_DOUBLE,                /* double type */
    MCC_FEAT_LONG_DOUBLE,           /* long double type */
    MCC_FEAT_STRUCT_INIT,           /* Struct initializers */
    MCC_FEAT_ARRAY_INIT,            /* Array initializers */
    MCC_FEAT_UNION_INIT,            /* Union initializers (first member only in C89) */
    MCC_FEAT_BITFIELDS,             /* Bit-field members */
    MCC_FEAT_INCOMPLETE_TYPES,      /* Incomplete types */
    
    /* C89 Control Flow (24-31) */
    MCC_FEAT_IF_ELSE,               /* if/else statements */
    MCC_FEAT_SWITCH,                /* switch statement */
    MCC_FEAT_CASE,                  /* case labels */
    MCC_FEAT_DEFAULT,               /* default label */
    MCC_FEAT_WHILE,                 /* while loops */
    MCC_FEAT_DO_WHILE,              /* do-while loops */
    MCC_FEAT_FOR,                   /* for loops */
    MCC_FEAT_GOTO,                  /* goto statements */
    
    /* C89 Control Flow continued (32-39) */
    MCC_FEAT_BREAK,                 /* break statement */
    MCC_FEAT_CONTINUE,              /* continue statement */
    MCC_FEAT_RETURN,                /* return statement */
    MCC_FEAT_LABELS,                /* Named labels */
    MCC_FEAT_COMPOUND_STMT,         /* Compound statements {} */
    MCC_FEAT_EMPTY_STMT,            /* Empty statement ; */
    MCC_FEAT_EXPR_STMT,             /* Expression statements */
    MCC_FEAT_NULL_STMT,             /* Null statements */
    
    /* C89 Operators (40-55) */
    MCC_FEAT_OP_ADD,                /* + */
    MCC_FEAT_OP_SUB,                /* - */
    MCC_FEAT_OP_MUL,                /* * */
    MCC_FEAT_OP_DIV,                /* / */
    MCC_FEAT_OP_MOD,                /* % */
    MCC_FEAT_OP_AND,                /* & (bitwise) */
    MCC_FEAT_OP_OR,                 /* | (bitwise) */
    MCC_FEAT_OP_XOR,                /* ^ (bitwise) */
    MCC_FEAT_OP_NOT,                /* ~ (bitwise) */
    MCC_FEAT_OP_LSHIFT,             /* << */
    MCC_FEAT_OP_RSHIFT,             /* >> */
    MCC_FEAT_OP_LOG_AND,            /* && */
    MCC_FEAT_OP_LOG_OR,             /* || */
    MCC_FEAT_OP_LOG_NOT,            /* ! */
    MCC_FEAT_OP_EQ,                 /* == */
    MCC_FEAT_OP_NE,                 /* != */
    
    /* C89 Operators continued (56-63) */
    MCC_FEAT_OP_LT,                 /* < */
    MCC_FEAT_OP_GT,                 /* > */
    MCC_FEAT_OP_LE,                 /* <= */
    MCC_FEAT_OP_GE,                 /* >= */
    MCC_FEAT_OP_ASSIGN,             /* = */
    MCC_FEAT_OP_COMPOUND_ASSIGN,    /* +=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>= */
    MCC_FEAT_OP_INC,                /* ++ */
    MCC_FEAT_OP_DEC,                /* -- */
    
    /* ============================================================
     * Word 1: C89 continued + C99 Features (64-127)
     * ============================================================ */
    
    /* C89 Operators continued (64-71) */
    MCC_FEAT_OP_TERNARY = 64,       /* ?: */
    MCC_FEAT_OP_COMMA,              /* , */
    MCC_FEAT_OP_SIZEOF,             /* sizeof */
    MCC_FEAT_OP_CAST,               /* (type) */
    MCC_FEAT_OP_ADDR,               /* & (address-of) */
    MCC_FEAT_OP_DEREF,              /* * (dereference) */
    MCC_FEAT_OP_MEMBER,             /* . */
    MCC_FEAT_OP_ARROW,              /* -> */
    
    /* C89 Operators continued (72-79) */
    MCC_FEAT_OP_SUBSCRIPT,          /* [] */
    MCC_FEAT_OP_CALL,               /* () function call */
    MCC_FEAT_OP_UNARY_PLUS,         /* unary + */
    MCC_FEAT_OP_UNARY_MINUS,        /* unary - */
    MCC_FEAT_RESERVED_72,
    MCC_FEAT_RESERVED_73,
    MCC_FEAT_RESERVED_74,
    MCC_FEAT_RESERVED_75,
    
    /* C89 Preprocessor (80-95) */
    MCC_FEAT_PP_DEFINE = 80,        /* #define */
    MCC_FEAT_PP_UNDEF,              /* #undef */
    MCC_FEAT_PP_INCLUDE,            /* #include */
    MCC_FEAT_PP_INCLUDE_NEXT,       /* #include_next (GNU) */
    MCC_FEAT_PP_IF,                 /* #if */
    MCC_FEAT_PP_IFDEF,              /* #ifdef */
    MCC_FEAT_PP_IFNDEF,             /* #ifndef */
    MCC_FEAT_PP_ELIF,               /* #elif */
    MCC_FEAT_PP_ELSE,               /* #else */
    MCC_FEAT_PP_ENDIF,              /* #endif */
    MCC_FEAT_PP_ERROR,              /* #error */
    MCC_FEAT_PP_WARNING,            /* #warning (GNU) */
    MCC_FEAT_PP_PRAGMA,             /* #pragma */
    MCC_FEAT_PP_LINE,               /* #line */
    MCC_FEAT_PP_DEFINED,            /* defined() operator */
    MCC_FEAT_PP_STRINGIFY,          /* # stringification */
    
    /* C89 Preprocessor continued (96-103) */
    MCC_FEAT_PP_CONCAT,             /* ## token pasting */
    MCC_FEAT_PP_FUNC_MACRO,         /* Function-like macros */
    MCC_FEAT_PP_OBJ_MACRO,          /* Object-like macros */
    MCC_FEAT_PP_PREDEFINED,         /* Predefined macros (__FILE__, __LINE__, etc.) */
    MCC_FEAT_RESERVED_100,
    MCC_FEAT_RESERVED_101,
    MCC_FEAT_RESERVED_102,
    MCC_FEAT_RESERVED_103,
    
    /* C89 Other (104-111) */
    MCC_FEAT_FUNC_PROTO = 104,      /* Function prototypes */
    MCC_FEAT_FUNC_DEF,              /* Function definitions */
    MCC_FEAT_FUNC_DECL,             /* Function declarations */
    MCC_FEAT_ELLIPSIS,              /* Variadic functions (...) */
    MCC_FEAT_STRING_LIT,            /* String literals */
    MCC_FEAT_CHAR_LIT,              /* Character literals */
    MCC_FEAT_INT_LIT,               /* Integer literals */
    MCC_FEAT_FLOAT_LIT,             /* Floating-point literals */
    
    /* C89 Other continued (112-119) */
    MCC_FEAT_OCTAL_LIT,             /* Octal literals */
    MCC_FEAT_HEX_LIT,               /* Hexadecimal literals */
    MCC_FEAT_ESCAPE_SEQ,            /* Escape sequences */
    MCC_FEAT_BLOCK_COMMENT,         /* Block comments */
    MCC_FEAT_EXTERN,                /* extern storage class */
    MCC_FEAT_STATIC,                /* static storage class */
    MCC_FEAT_AUTO,                  /* auto storage class */
    MCC_FEAT_REGISTER,              /* register storage class */
    
    /* C99 Types (120-127) */
    MCC_FEAT_LONG_LONG = 120,       /* long long int */
    MCC_FEAT_BOOL,                  /* _Bool type */
    MCC_FEAT_COMPLEX,               /* _Complex type */
    MCC_FEAT_IMAGINARY,             /* _Imaginary type */
    MCC_FEAT_RESTRICT,              /* restrict qualifier */
    MCC_FEAT_INLINE,                /* inline functions */
    MCC_FEAT_STDINT_TYPES,          /* <stdint.h> types (int8_t, etc.) */
    MCC_FEAT_STDBOOL,               /* <stdbool.h> (bool, true, false) */
    
    /* ============================================================
     * Word 2: C99 continued + C11 Features (128-191)
     * ============================================================ */
    
    /* C99 Declarations (128-143) */
    MCC_FEAT_MIXED_DECL = 128,      /* Mixed declarations and code */
    MCC_FEAT_FOR_DECL,              /* Declarations in for loop init */
    MCC_FEAT_VLA,                   /* Variable Length Arrays */
    MCC_FEAT_FLEXIBLE_ARRAY,        /* Flexible array members */
    MCC_FEAT_DESIGNATED_INIT,       /* Designated initializers */
    MCC_FEAT_COMPOUND_LIT,          /* Compound literals */
    MCC_FEAT_STATIC_ASSERT_C11,     /* _Static_assert (C11) */
    MCC_FEAT_INIT_EXPR,             /* Non-constant initializers */
    MCC_FEAT_ARRAY_DESIGNATOR,      /* [index] = value */
    MCC_FEAT_STRUCT_DESIGNATOR,     /* .member = value */
    MCC_FEAT_NESTED_DESIGNATOR,     /* .member[i].field = value */
    MCC_FEAT_RESERVED_139,
    MCC_FEAT_RESERVED_140,
    MCC_FEAT_RESERVED_141,
    MCC_FEAT_RESERVED_142,
    MCC_FEAT_RESERVED_143,
    
    /* C99 Preprocessor (144-159) */
    MCC_FEAT_PP_VARIADIC = 144,     /* Variadic macros (__VA_ARGS__) */
    MCC_FEAT_PP_VA_ARGS,            /* __VA_ARGS__ identifier */
    MCC_FEAT_PP_PRAGMA_OP,          /* _Pragma operator */
    MCC_FEAT_PP_EMPTY_ARGS,         /* Empty macro arguments */
    MCC_FEAT_PP_VA_OPT,             /* __VA_OPT__ (C23) */
    MCC_FEAT_PP_ELIFDEF,            /* #elifdef (C23) */
    MCC_FEAT_PP_ELIFNDEF,           /* #elifndef (C23) */
    MCC_FEAT_PP_EMBED,              /* #embed (C23) */
    MCC_FEAT_RESERVED_152,
    MCC_FEAT_RESERVED_153,
    MCC_FEAT_RESERVED_154,
    MCC_FEAT_RESERVED_155,
    MCC_FEAT_RESERVED_156,
    MCC_FEAT_RESERVED_157,
    MCC_FEAT_RESERVED_158,
    MCC_FEAT_RESERVED_159,
    
    /* C99 Other (160-175) */
    MCC_FEAT_LINE_COMMENT = 160,    /* // comments */
    MCC_FEAT_FUNC_NAME,             /* __func__ predefined identifier */
    MCC_FEAT_UNIVERSAL_CHAR,        /* Universal character names \uXXXX */
    MCC_FEAT_HEX_FLOAT,             /* Hexadecimal floating constants */
    MCC_FEAT_LONG_LONG_LIT,         /* LL/ULL suffixes */
    MCC_FEAT_INIT_STRUCT_ANON,      /* Anonymous struct/union init */
    MCC_FEAT_SNPRINTF,              /* snprintf (library, not language) */
    MCC_FEAT_RESERVED_167,
    MCC_FEAT_RESERVED_168,
    MCC_FEAT_RESERVED_169,
    MCC_FEAT_RESERVED_170,
    MCC_FEAT_RESERVED_171,
    MCC_FEAT_RESERVED_172,
    MCC_FEAT_RESERVED_173,
    MCC_FEAT_RESERVED_174,
    MCC_FEAT_RESERVED_175,
    
    /* C11 Features (176-191) */
    MCC_FEAT_ALIGNAS = 176,         /* _Alignas */
    MCC_FEAT_ALIGNOF,               /* _Alignof */
    MCC_FEAT_NORETURN,              /* _Noreturn */
    MCC_FEAT_STATIC_ASSERT,         /* _Static_assert */
    MCC_FEAT_GENERIC,               /* _Generic */
    MCC_FEAT_ATOMIC,                /* _Atomic */
    MCC_FEAT_THREAD_LOCAL,          /* _Thread_local */
    MCC_FEAT_CHAR16_T,              /* char16_t */
    MCC_FEAT_CHAR32_T,              /* char32_t */
    MCC_FEAT_UNICODE_LIT,           /* u"" and U"" string literals */
    MCC_FEAT_ANONYMOUS_STRUCT,      /* Anonymous structs/unions */
    MCC_FEAT_RESERVED_187,
    MCC_FEAT_RESERVED_188,
    MCC_FEAT_RESERVED_189,
    MCC_FEAT_RESERVED_190,
    MCC_FEAT_RESERVED_191,
    
    /* ============================================================
     * Word 3: C17/C23 + GNU Extensions (192-255)
     * ============================================================ */
    
    /* C17 Features (192-199) - mostly defect fixes, few new features */
    MCC_FEAT_C17_DEPRECATED = 192,  /* [[deprecated]] attribute */
    MCC_FEAT_RESERVED_193,
    MCC_FEAT_RESERVED_194,
    MCC_FEAT_RESERVED_195,
    MCC_FEAT_RESERVED_196,
    MCC_FEAT_RESERVED_197,
    MCC_FEAT_RESERVED_198,
    MCC_FEAT_RESERVED_199,
    
    /* C23 Features (200-223) */
    MCC_FEAT_NULLPTR = 200,         /* nullptr constant */
    MCC_FEAT_CONSTEXPR,             /* constexpr */
    MCC_FEAT_TYPEOF,                /* typeof */
    MCC_FEAT_TYPEOF_UNQUAL,         /* typeof_unqual */
    MCC_FEAT_AUTO_TYPE,             /* auto type inference */
    MCC_FEAT_BOOL_KEYWORD,          /* bool as keyword (not _Bool) */
    MCC_FEAT_TRUE_FALSE,            /* true/false as keywords */
    MCC_FEAT_STATIC_ASSERT_MSG,     /* static_assert without message */
    MCC_FEAT_BINARY_LIT,            /* 0b binary literals */
    MCC_FEAT_DIGIT_SEP,             /* ' digit separator */
    MCC_FEAT_ATTR_SYNTAX,           /* [[attribute]] syntax */
    MCC_FEAT_ATTR_DEPRECATED,       /* [[deprecated]] */
    MCC_FEAT_ATTR_FALLTHROUGH,      /* [[fallthrough]] */
    MCC_FEAT_ATTR_MAYBE_UNUSED,     /* [[maybe_unused]] */
    MCC_FEAT_ATTR_NODISCARD,        /* [[nodiscard]] */
    MCC_FEAT_ATTR_NORETURN,         /* [[noreturn]] */
    MCC_FEAT_UNREACHABLE,           /* unreachable() */
    MCC_FEAT_CHAR8_T,               /* char8_t */
    MCC_FEAT_UTF8_CHAR_LIT,         /* u8'x' */
    MCC_FEAT_RESERVED_219,
    MCC_FEAT_RESERVED_220,
    MCC_FEAT_RESERVED_221,
    MCC_FEAT_RESERVED_222,
    MCC_FEAT_RESERVED_223,
    
    /* GNU Extensions (224-255) */
    MCC_FEAT_GNU_EXT = 224,         /* General GNU extensions flag */
    MCC_FEAT_GNU_ASM,               /* GNU inline assembly */
    MCC_FEAT_GNU_ATTR,              /* __attribute__ */
    MCC_FEAT_GNU_TYPEOF,            /* __typeof__ */
    MCC_FEAT_GNU_STMT_EXPR,         /* Statement expressions ({ }) */
    MCC_FEAT_GNU_LABEL_ADDR,        /* Labels as values && */
    MCC_FEAT_GNU_CASE_RANGE,        /* case 1 ... 5: */
    MCC_FEAT_GNU_ZERO_ARRAY,        /* Zero-length arrays */
    MCC_FEAT_GNU_EMPTY_STRUCT,      /* Empty structs */
    MCC_FEAT_GNU_NESTED_FUNC,       /* Nested functions */
    MCC_FEAT_GNU_BUILTIN,           /* __builtin_* functions */
    MCC_FEAT_GNU_ALIGNOF,           /* __alignof__ */
    MCC_FEAT_GNU_EXTENSION,         /* __extension__ */
    MCC_FEAT_GNU_INLINE,            /* GNU inline semantics */
    MCC_FEAT_GNU_COMPLEX,           /* __complex__ */
    MCC_FEAT_GNU_REAL_IMAG,         /* __real__ and __imag__ */
    MCC_FEAT_GNU_VECTOR,            /* Vector extensions */
    MCC_FEAT_GNU_INIT_PRIORITY,     /* Constructor priorities */
    MCC_FEAT_GNU_VISIBILITY,        /* Visibility attributes */
    MCC_FEAT_GNU_CLEANUP,           /* cleanup attribute */
    MCC_FEAT_GNU_PACKED,            /* packed attribute */
    MCC_FEAT_GNU_ALIGNED,           /* aligned attribute */
    MCC_FEAT_GNU_SECTION,           /* section attribute */
    MCC_FEAT_GNU_WEAK,              /* weak attribute */
    MCC_FEAT_GNU_ALIAS,             /* alias attribute */
    MCC_FEAT_GNU_DEPRECATED,        /* deprecated attribute */
    MCC_FEAT_GNU_UNUSED,            /* unused attribute */
    MCC_FEAT_GNU_FORMAT,            /* format attribute */
    MCC_FEAT_GNU_NONNULL,           /* nonnull attribute */
    MCC_FEAT_GNU_SENTINEL,          /* sentinel attribute */
    MCC_FEAT_GNU_MALLOC,            /* malloc attribute */
    MCC_FEAT_GNU_PURE,              /* pure attribute */
    
    /* Total count */
    MCC_FEAT_COUNT = 256
} mcc_feature_id_t;

/* ============================================================
 * Feature Manipulation Macros
 * ============================================================ */

/* Get word index and bit position for a feature */
#define MCC_FEAT_WORD(feat)  ((feat) / MCC_FEATURE_BITS)
#define MCC_FEAT_BIT(feat)   ((feat) % MCC_FEATURE_BITS)
#define MCC_FEAT_MASK(feat)  (1ULL << MCC_FEAT_BIT(feat))

/* Initialize features to zero */
#define MCC_FEATURES_INIT(f) memset(&(f), 0, sizeof(mcc_c_features_t))

/* Set a single feature */
#define MCC_FEATURES_SET(f, feat) \
    ((f).bits[MCC_FEAT_WORD(feat)] |= MCC_FEAT_MASK(feat))

/* Clear a single feature */
#define MCC_FEATURES_CLEAR(f, feat) \
    ((f).bits[MCC_FEAT_WORD(feat)] &= ~MCC_FEAT_MASK(feat))

/* Test if a feature is set */
#define MCC_FEATURES_HAS(f, feat) \
    (((f).bits[MCC_FEAT_WORD(feat)] & MCC_FEAT_MASK(feat)) != 0)

/* Copy features */
#define MCC_FEATURES_COPY(dst, src) memcpy(&(dst), &(src), sizeof(mcc_c_features_t))

/* ============================================================
 * Feature Set Operations (functions for complex operations)
 * ============================================================ */

/* Combine two feature sets (OR) */
static inline void mcc_features_or(mcc_c_features_t *dst, const mcc_c_features_t *src)
{
    for (int i = 0; i < MCC_FEATURE_WORDS; i++) {
        dst->bits[i] |= src->bits[i];
    }
}

/* Intersect two feature sets (AND) */
static inline void mcc_features_and(mcc_c_features_t *dst, const mcc_c_features_t *src)
{
    for (int i = 0; i < MCC_FEATURE_WORDS; i++) {
        dst->bits[i] &= src->bits[i];
    }
}

/* Remove features (AND NOT) */
static inline void mcc_features_remove(mcc_c_features_t *dst, const mcc_c_features_t *src)
{
    for (int i = 0; i < MCC_FEATURE_WORDS; i++) {
        dst->bits[i] &= ~src->bits[i];
    }
}

/* Check if all features in 'required' are present in 'features' */
static inline bool mcc_features_has_all(const mcc_c_features_t *features, 
                                         const mcc_c_features_t *required)
{
    for (int i = 0; i < MCC_FEATURE_WORDS; i++) {
        if ((features->bits[i] & required->bits[i]) != required->bits[i]) {
            return false;
        }
    }
    return true;
}

/* Check if any feature in 'check' is present in 'features' */
static inline bool mcc_features_has_any(const mcc_c_features_t *features,
                                         const mcc_c_features_t *check)
{
    for (int i = 0; i < MCC_FEATURE_WORDS; i++) {
        if (features->bits[i] & check->bits[i]) {
            return true;
        }
    }
    return false;
}

/* Check if feature set is empty */
static inline bool mcc_features_is_empty(const mcc_c_features_t *features)
{
    for (int i = 0; i < MCC_FEATURE_WORDS; i++) {
        if (features->bits[i] != 0) {
            return false;
        }
    }
    return true;
}

/*
 * C Standard Information Structure
 * Similar to anvil_arch_info_t
 */
typedef struct mcc_c_std_info {
    mcc_c_std_t std;                /* Standard enum value */
    const char *name;               /* Short name (e.g., "c99") */
    const char *description;        /* Full description */
    int year;                       /* Year of standard (e.g., 1989, 1999) */
    const char *iso_name;           /* ISO standard name (e.g., "ISO/IEC 9899:1999") */
    mcc_c_std_t base_std;           /* Base standard (for GNU variants) */
    bool is_gnu;                    /* Is this a GNU extension variant? */
} mcc_c_std_info_t;

/*
 * Predefined macro structure
 */
typedef struct mcc_predefined_macro {
    const char *name;
    const char *value;              /* NULL for object-like macros with no value */
} mcc_predefined_macro_t;

/*
 * API Functions
 */

/* Get information about a C standard */
const mcc_c_std_info_t *mcc_c_std_get_info(mcc_c_std_t std);

/* Get standard from name (e.g., "c99", "gnu89") */
mcc_c_std_t mcc_c_std_from_name(const char *name);

/* Get name from standard */
const char *mcc_c_std_get_name(mcc_c_std_t std);

/* Get the effective standard (resolve DEFAULT) */
mcc_c_std_t mcc_c_std_resolve(mcc_c_std_t std);

/* Get features for a standard (fills the provided structure) */
void mcc_c_std_get_features(mcc_c_std_t std, mcc_c_features_t *features);

/* Check if a feature is available in a standard */
bool mcc_c_std_has_feature(mcc_c_std_t std, mcc_feature_id_t feature);

/* Get the base standard (e.g., GNU99 -> C99) */
mcc_c_std_t mcc_c_std_get_base(mcc_c_std_t std);

/* Check if standard is a GNU variant */
bool mcc_c_std_is_gnu(mcc_c_std_t std);

/* Compare standards (returns -1, 0, or 1) */
int mcc_c_std_compare(mcc_c_std_t a, mcc_c_std_t b);

/* Get predefined macros for a standard */
const mcc_predefined_macro_t *mcc_c_std_get_predefined_macros(mcc_c_std_t std, size_t *count);

/* Initialize feature set for a standard */
void mcc_c_std_init_features(mcc_c_std_t std, mcc_c_features_t *features);

#endif /* MCC_C_STD_H */
