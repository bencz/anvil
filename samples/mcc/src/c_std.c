/*
 * MCC - Micro C Compiler
 * C Language Standard Implementation
 */

#include "mcc.h"
#include "c_std.h"
#include <string.h>

/*
 * C Standard Information Table
 * Similar to ANVIL's cpu_model_table and arch_info_table
 */
static const mcc_c_std_info_t c_std_info_table[] = {
    /* Default - resolves to C89 */
    {
        .std = MCC_STD_DEFAULT,
        .name = "default",
        .description = "Compiler default (C89)",
        .year = 1989,
        .iso_name = NULL,
        .base_std = MCC_STD_C89,
        .is_gnu = false
    },
    
    /* C89 - ANSI C */
    {
        .std = MCC_STD_C89,
        .name = "c89",
        .description = "ANSI C (X3.159-1989)",
        .year = 1989,
        .iso_name = "ANSI X3.159-1989",
        .base_std = MCC_STD_C89,
        .is_gnu = false
    },
    
    /* C90 - ISO C (identical to C89) */
    {
        .std = MCC_STD_C90,
        .name = "c90",
        .description = "ISO C90 (identical to C89)",
        .year = 1990,
        .iso_name = "ISO/IEC 9899:1990",
        .base_std = MCC_STD_C89,
        .is_gnu = false
    },
    
    /* C99 */
    {
        .std = MCC_STD_C99,
        .name = "c99",
        .description = "ISO C99",
        .year = 1999,
        .iso_name = "ISO/IEC 9899:1999",
        .base_std = MCC_STD_C99,
        .is_gnu = false
    },
    
    /* C11 (future) */
    {
        .std = MCC_STD_C11,
        .name = "c11",
        .description = "ISO C11",
        .year = 2011,
        .iso_name = "ISO/IEC 9899:2011",
        .base_std = MCC_STD_C11,
        .is_gnu = false
    },
    
    /* C17 (future) */
    {
        .std = MCC_STD_C17,
        .name = "c17",
        .description = "ISO C17",
        .year = 2017,
        .iso_name = "ISO/IEC 9899:2018",
        .base_std = MCC_STD_C17,
        .is_gnu = false
    },
    
    /* C23 (future) */
    {
        .std = MCC_STD_C23,
        .name = "c23",
        .description = "ISO C23",
        .year = 2023,
        .iso_name = "ISO/IEC 9899:2024",
        .base_std = MCC_STD_C23,
        .is_gnu = false
    },
    
    /* GNU89 */
    {
        .std = MCC_STD_GNU89,
        .name = "gnu89",
        .description = "GNU dialect of C89",
        .year = 1989,
        .iso_name = NULL,
        .base_std = MCC_STD_C89,
        .is_gnu = true
    },
    
    /* GNU99 */
    {
        .std = MCC_STD_GNU99,
        .name = "gnu99",
        .description = "GNU dialect of C99",
        .year = 1999,
        .iso_name = NULL,
        .base_std = MCC_STD_C99,
        .is_gnu = true
    },
    
    /* GNU11 (future) */
    {
        .std = MCC_STD_GNU11,
        .name = "gnu11",
        .description = "GNU dialect of C11",
        .year = 2011,
        .iso_name = NULL,
        .base_std = MCC_STD_C11,
        .is_gnu = true
    },
    
    /* Sentinel */
    {
        .std = MCC_STD_COUNT,
        .name = NULL,
        .description = NULL,
        .year = 0,
        .iso_name = NULL,
        .base_std = MCC_STD_DEFAULT,
        .is_gnu = false
    }
};

/*
 * Name lookup table for faster string matching
 * Includes common aliases
 */
static const struct {
    const char *name;
    mcc_c_std_t std;
} c_std_name_table[] = {
    /* Standard names */
    { "c89",        MCC_STD_C89 },
    { "c90",        MCC_STD_C90 },
    { "c99",        MCC_STD_C99 },
    { "c11",        MCC_STD_C11 },
    { "c17",        MCC_STD_C17 },
    { "c18",        MCC_STD_C17 },      /* C18 is an alias for C17 */
    { "c23",        MCC_STD_C23 },
    { "c2x",        MCC_STD_C23 },      /* C2x was the working name for C23 */
    
    /* GNU variants */
    { "gnu89",      MCC_STD_GNU89 },
    { "gnu90",      MCC_STD_GNU89 },    /* gnu90 is alias for gnu89 */
    { "gnu99",      MCC_STD_GNU99 },
    { "gnu9x",      MCC_STD_GNU99 },    /* gnu9x was working name */
    { "gnu11",      MCC_STD_GNU11 },
    { "gnu1x",      MCC_STD_GNU11 },    /* gnu1x was working name */
    
    /* ISO names */
    { "iso9899:1990",   MCC_STD_C90 },
    { "iso9899:199409", MCC_STD_C90 },  /* C90 with Amendment 1 */
    { "iso9899:1999",   MCC_STD_C99 },
    { "iso9899:2011",   MCC_STD_C11 },
    { "iso9899:2017",   MCC_STD_C17 },
    { "iso9899:2018",   MCC_STD_C17 },
    
    /* Default/special */
    { "default",    MCC_STD_DEFAULT },
    
    /* Sentinel */
    { NULL, MCC_STD_COUNT }
};

/*
 * Helper function to set multiple features at once
 */
static void set_features(mcc_c_features_t *f, const mcc_feature_id_t *features, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        MCC_FEATURES_SET(*f, features[i]);
    }
}

/*
 * Initialize C89 features
 */
static void init_c89_features(mcc_c_features_t *f)
{
    MCC_FEATURES_INIT(*f);
    
    /* C89 Types */
    static const mcc_feature_id_t c89_types[] = {
        MCC_FEAT_BASIC_TYPES, MCC_FEAT_POINTERS, MCC_FEAT_ARRAYS,
        MCC_FEAT_STRUCTS, MCC_FEAT_UNIONS, MCC_FEAT_ENUMS,
        MCC_FEAT_TYPEDEF, MCC_FEAT_CONST, MCC_FEAT_VOLATILE,
        MCC_FEAT_SIGNED, MCC_FEAT_UNSIGNED, MCC_FEAT_VOID,
        MCC_FEAT_CHAR, MCC_FEAT_SHORT, MCC_FEAT_INT, MCC_FEAT_LONG,
        MCC_FEAT_FLOAT, MCC_FEAT_DOUBLE, MCC_FEAT_LONG_DOUBLE,
        MCC_FEAT_STRUCT_INIT, MCC_FEAT_ARRAY_INIT, MCC_FEAT_UNION_INIT,
        MCC_FEAT_BITFIELDS, MCC_FEAT_INCOMPLETE_TYPES
    };
    set_features(f, c89_types, sizeof(c89_types)/sizeof(c89_types[0]));
    
    /* C89 Control Flow */
    static const mcc_feature_id_t c89_control[] = {
        MCC_FEAT_IF_ELSE, MCC_FEAT_SWITCH, MCC_FEAT_CASE, MCC_FEAT_DEFAULT,
        MCC_FEAT_WHILE, MCC_FEAT_DO_WHILE, MCC_FEAT_FOR, MCC_FEAT_GOTO,
        MCC_FEAT_BREAK, MCC_FEAT_CONTINUE, MCC_FEAT_RETURN, MCC_FEAT_LABELS,
        MCC_FEAT_COMPOUND_STMT, MCC_FEAT_EMPTY_STMT, MCC_FEAT_EXPR_STMT, MCC_FEAT_NULL_STMT
    };
    set_features(f, c89_control, sizeof(c89_control)/sizeof(c89_control[0]));
    
    /* C89 Operators */
    static const mcc_feature_id_t c89_ops[] = {
        MCC_FEAT_OP_ADD, MCC_FEAT_OP_SUB, MCC_FEAT_OP_MUL, MCC_FEAT_OP_DIV, MCC_FEAT_OP_MOD,
        MCC_FEAT_OP_AND, MCC_FEAT_OP_OR, MCC_FEAT_OP_XOR, MCC_FEAT_OP_NOT,
        MCC_FEAT_OP_LSHIFT, MCC_FEAT_OP_RSHIFT,
        MCC_FEAT_OP_LOG_AND, MCC_FEAT_OP_LOG_OR, MCC_FEAT_OP_LOG_NOT,
        MCC_FEAT_OP_EQ, MCC_FEAT_OP_NE, MCC_FEAT_OP_LT, MCC_FEAT_OP_GT, MCC_FEAT_OP_LE, MCC_FEAT_OP_GE,
        MCC_FEAT_OP_ASSIGN, MCC_FEAT_OP_COMPOUND_ASSIGN, MCC_FEAT_OP_INC, MCC_FEAT_OP_DEC,
        MCC_FEAT_OP_TERNARY, MCC_FEAT_OP_COMMA, MCC_FEAT_OP_SIZEOF, MCC_FEAT_OP_CAST,
        MCC_FEAT_OP_ADDR, MCC_FEAT_OP_DEREF, MCC_FEAT_OP_MEMBER, MCC_FEAT_OP_ARROW,
        MCC_FEAT_OP_SUBSCRIPT, MCC_FEAT_OP_CALL, MCC_FEAT_OP_UNARY_PLUS, MCC_FEAT_OP_UNARY_MINUS
    };
    set_features(f, c89_ops, sizeof(c89_ops)/sizeof(c89_ops[0]));
    
    /* C89 Preprocessor */
    static const mcc_feature_id_t c89_pp[] = {
        MCC_FEAT_PP_DEFINE, MCC_FEAT_PP_UNDEF, MCC_FEAT_PP_INCLUDE,
        MCC_FEAT_PP_IF, MCC_FEAT_PP_IFDEF, MCC_FEAT_PP_IFNDEF,
        MCC_FEAT_PP_ELIF, MCC_FEAT_PP_ELSE, MCC_FEAT_PP_ENDIF,
        MCC_FEAT_PP_ERROR, MCC_FEAT_PP_PRAGMA, MCC_FEAT_PP_LINE,
        MCC_FEAT_PP_DEFINED, MCC_FEAT_PP_STRINGIFY, MCC_FEAT_PP_CONCAT,
        MCC_FEAT_PP_FUNC_MACRO, MCC_FEAT_PP_OBJ_MACRO, MCC_FEAT_PP_PREDEFINED
    };
    set_features(f, c89_pp, sizeof(c89_pp)/sizeof(c89_pp[0]));
    
    /* C89 Other */
    static const mcc_feature_id_t c89_other[] = {
        MCC_FEAT_FUNC_PROTO, MCC_FEAT_FUNC_DEF, MCC_FEAT_FUNC_DECL, MCC_FEAT_ELLIPSIS,
        MCC_FEAT_STRING_LIT, MCC_FEAT_CHAR_LIT, MCC_FEAT_INT_LIT, MCC_FEAT_FLOAT_LIT,
        MCC_FEAT_OCTAL_LIT, MCC_FEAT_HEX_LIT, MCC_FEAT_ESCAPE_SEQ, MCC_FEAT_BLOCK_COMMENT,
        MCC_FEAT_EXTERN, MCC_FEAT_STATIC, MCC_FEAT_AUTO, MCC_FEAT_REGISTER
    };
    set_features(f, c89_other, sizeof(c89_other)/sizeof(c89_other[0]));
}

/*
 * Initialize C99 features (adds to C89)
 */
static void init_c99_features(mcc_c_features_t *f)
{
    /* Start with C89 */
    init_c89_features(f);
    
    /* C99 Types */
    static const mcc_feature_id_t c99_types[] = {
        MCC_FEAT_LONG_LONG, MCC_FEAT_BOOL, MCC_FEAT_COMPLEX, MCC_FEAT_IMAGINARY,
        MCC_FEAT_RESTRICT, MCC_FEAT_INLINE, MCC_FEAT_STDINT_TYPES, MCC_FEAT_STDBOOL
    };
    set_features(f, c99_types, sizeof(c99_types)/sizeof(c99_types[0]));
    
    /* C99 Declarations */
    static const mcc_feature_id_t c99_decl[] = {
        MCC_FEAT_MIXED_DECL, MCC_FEAT_FOR_DECL, MCC_FEAT_VLA, MCC_FEAT_FLEXIBLE_ARRAY,
        MCC_FEAT_DESIGNATED_INIT, MCC_FEAT_COMPOUND_LIT, MCC_FEAT_INIT_EXPR,
        MCC_FEAT_ARRAY_DESIGNATOR, MCC_FEAT_STRUCT_DESIGNATOR, MCC_FEAT_NESTED_DESIGNATOR
    };
    set_features(f, c99_decl, sizeof(c99_decl)/sizeof(c99_decl[0]));
    
    /* C99 Preprocessor */
    static const mcc_feature_id_t c99_pp[] = {
        MCC_FEAT_PP_VARIADIC, MCC_FEAT_PP_VA_ARGS, MCC_FEAT_PP_PRAGMA_OP, MCC_FEAT_PP_EMPTY_ARGS
    };
    set_features(f, c99_pp, sizeof(c99_pp)/sizeof(c99_pp[0]));
    
    /* C99 Other */
    static const mcc_feature_id_t c99_other[] = {
        MCC_FEAT_LINE_COMMENT, MCC_FEAT_FUNC_NAME, MCC_FEAT_UNIVERSAL_CHAR,
        MCC_FEAT_HEX_FLOAT, MCC_FEAT_LONG_LONG_LIT
    };
    set_features(f, c99_other, sizeof(c99_other)/sizeof(c99_other[0]));
}

/*
 * Initialize C11 features (adds to C99)
 */
static void init_c11_features(mcc_c_features_t *f)
{
    /* Start with C99 */
    init_c99_features(f);
    
    /* C11 Features */
    static const mcc_feature_id_t c11_features[] = {
        MCC_FEAT_ALIGNAS, MCC_FEAT_ALIGNOF, MCC_FEAT_NORETURN, MCC_FEAT_STATIC_ASSERT,
        MCC_FEAT_GENERIC, MCC_FEAT_ATOMIC, MCC_FEAT_THREAD_LOCAL,
        MCC_FEAT_CHAR16_T, MCC_FEAT_CHAR32_T, MCC_FEAT_UNICODE_LIT, MCC_FEAT_ANONYMOUS_STRUCT
    };
    set_features(f, c11_features, sizeof(c11_features)/sizeof(c11_features[0]));
}

/*
 * Initialize C17 features (same as C11, mostly defect fixes)
 */
static void init_c17_features(mcc_c_features_t *f)
{
    /* C17 is essentially C11 with defect fixes */
    init_c11_features(f);
}

/*
 * Initialize C23 features (adds to C17)
 */
static void init_c23_features(mcc_c_features_t *f)
{
    /* Start with C17 */
    init_c17_features(f);
    
    /* C23 Features */
    static const mcc_feature_id_t c23_features[] = {
        MCC_FEAT_NULLPTR, MCC_FEAT_CONSTEXPR, MCC_FEAT_TYPEOF, MCC_FEAT_TYPEOF_UNQUAL,
        MCC_FEAT_AUTO_TYPE, MCC_FEAT_BOOL_KEYWORD, MCC_FEAT_TRUE_FALSE, MCC_FEAT_STATIC_ASSERT_MSG,
        MCC_FEAT_BINARY_LIT, MCC_FEAT_DIGIT_SEP,
        MCC_FEAT_ATTR_SYNTAX, MCC_FEAT_ATTR_DEPRECATED, MCC_FEAT_ATTR_FALLTHROUGH,
        MCC_FEAT_ATTR_MAYBE_UNUSED, MCC_FEAT_ATTR_NODISCARD, MCC_FEAT_ATTR_NORETURN,
        MCC_FEAT_UNREACHABLE, MCC_FEAT_CHAR8_T, MCC_FEAT_UTF8_CHAR_LIT,
        MCC_FEAT_PP_VA_OPT, MCC_FEAT_PP_ELIFDEF, MCC_FEAT_PP_ELIFNDEF, MCC_FEAT_PP_EMBED
    };
    set_features(f, c23_features, sizeof(c23_features)/sizeof(c23_features[0]));
}

/*
 * Initialize GNU extension features
 */
static void init_gnu_features(mcc_c_features_t *f)
{
    static const mcc_feature_id_t gnu_features[] = {
        MCC_FEAT_GNU_EXT, MCC_FEAT_GNU_ASM, MCC_FEAT_GNU_ATTR, MCC_FEAT_GNU_TYPEOF,
        MCC_FEAT_GNU_STMT_EXPR, MCC_FEAT_GNU_LABEL_ADDR, MCC_FEAT_GNU_CASE_RANGE,
        MCC_FEAT_GNU_ZERO_ARRAY, MCC_FEAT_GNU_EMPTY_STRUCT, MCC_FEAT_GNU_NESTED_FUNC,
        MCC_FEAT_GNU_BUILTIN, MCC_FEAT_GNU_ALIGNOF, MCC_FEAT_GNU_EXTENSION, MCC_FEAT_GNU_INLINE,
        MCC_FEAT_GNU_COMPLEX, MCC_FEAT_GNU_REAL_IMAG, MCC_FEAT_GNU_VECTOR,
        MCC_FEAT_GNU_INIT_PRIORITY, MCC_FEAT_GNU_VISIBILITY, MCC_FEAT_GNU_CLEANUP,
        MCC_FEAT_GNU_PACKED, MCC_FEAT_GNU_ALIGNED, MCC_FEAT_GNU_SECTION, MCC_FEAT_GNU_WEAK,
        MCC_FEAT_GNU_ALIAS, MCC_FEAT_GNU_DEPRECATED, MCC_FEAT_GNU_UNUSED, MCC_FEAT_GNU_FORMAT,
        MCC_FEAT_GNU_NONNULL, MCC_FEAT_GNU_SENTINEL, MCC_FEAT_GNU_MALLOC, MCC_FEAT_GNU_PURE,
        MCC_FEAT_PP_INCLUDE_NEXT, MCC_FEAT_PP_WARNING, MCC_FEAT_LINE_COMMENT
    };
    set_features(f, gnu_features, sizeof(gnu_features)/sizeof(gnu_features[0]));
}

/*
 * Predefined macros for each standard
 */

/* C89/C90 predefined macros */
static const mcc_predefined_macro_t c89_predefined_macros[] = {
    { "__STDC__", "1" },
    { NULL, NULL }
};

/* C99 predefined macros */
static const mcc_predefined_macro_t c99_predefined_macros[] = {
    { "__STDC__", "1" },
    { "__STDC_VERSION__", "199901L" },
    { "__STDC_HOSTED__", "1" },
    { NULL, NULL }
};

/* C11 predefined macros */
static const mcc_predefined_macro_t c11_predefined_macros[] = {
    { "__STDC__", "1" },
    { "__STDC_VERSION__", "201112L" },
    { "__STDC_HOSTED__", "1" },
    { "__STDC_UTF_16__", "1" },
    { "__STDC_UTF_32__", "1" },
    { NULL, NULL }
};

/* C17 predefined macros */
static const mcc_predefined_macro_t c17_predefined_macros[] = {
    { "__STDC__", "1" },
    { "__STDC_VERSION__", "201710L" },
    { "__STDC_HOSTED__", "1" },
    { "__STDC_UTF_16__", "1" },
    { "__STDC_UTF_32__", "1" },
    { NULL, NULL }
};

/* C23 predefined macros */
static const mcc_predefined_macro_t c23_predefined_macros[] = {
    { "__STDC__", "1" },
    { "__STDC_VERSION__", "202311L" },
    { "__STDC_HOSTED__", "1" },
    { "__STDC_UTF_16__", "1" },
    { "__STDC_UTF_32__", "1" },
    { NULL, NULL }
};

/* GNU89 predefined macros */
static const mcc_predefined_macro_t gnu89_predefined_macros[] = {
    { "__STDC__", "1" },
    { "__GNUC__", "4" },
    { "__GNUC_MINOR__", "0" },
    { NULL, NULL }
};

/* GNU99 predefined macros */
static const mcc_predefined_macro_t gnu99_predefined_macros[] = {
    { "__STDC__", "1" },
    { "__STDC_VERSION__", "199901L" },
    { "__GNUC__", "4" },
    { "__GNUC_MINOR__", "0" },
    { NULL, NULL }
};

/*
 * API Implementation
 */

const mcc_c_std_info_t *mcc_c_std_get_info(mcc_c_std_t std)
{
    if (std >= MCC_STD_COUNT) {
        return NULL;
    }
    
    for (size_t i = 0; c_std_info_table[i].name != NULL; i++) {
        if (c_std_info_table[i].std == std) {
            return &c_std_info_table[i];
        }
    }
    
    return NULL;
}

mcc_c_std_t mcc_c_std_from_name(const char *name)
{
    if (!name) {
        return MCC_STD_DEFAULT;
    }
    
    /* Skip leading '-std=' if present */
    if (strncmp(name, "-std=", 5) == 0) {
        name += 5;
    }
    
    /* Search in name table */
    for (size_t i = 0; c_std_name_table[i].name != NULL; i++) {
        if (strcasecmp(c_std_name_table[i].name, name) == 0) {
            return c_std_name_table[i].std;
        }
    }
    
    return MCC_STD_DEFAULT;
}

const char *mcc_c_std_get_name(mcc_c_std_t std)
{
    const mcc_c_std_info_t *info = mcc_c_std_get_info(std);
    return info ? info->name : "unknown";
}

mcc_c_std_t mcc_c_std_resolve(mcc_c_std_t std)
{
    if (std == MCC_STD_DEFAULT) {
        return MCC_STD_C89;  /* Default to C89 */
    }
    return std;
}

void mcc_c_std_get_features(mcc_c_std_t std, mcc_c_features_t *features)
{
    mcc_c_std_init_features(std, features);
}

void mcc_c_std_init_features(mcc_c_std_t std, mcc_c_features_t *features)
{
    if (!features) return;
    
    std = mcc_c_std_resolve(std);
    
    switch (std) {
        case MCC_STD_C89:
        case MCC_STD_C90:
            init_c89_features(features);
            break;
            
        case MCC_STD_C99:
            init_c99_features(features);
            break;
            
        case MCC_STD_C11:
            init_c11_features(features);
            break;
            
        case MCC_STD_C17:
            init_c17_features(features);
            break;
            
        case MCC_STD_C23:
            init_c23_features(features);
            break;
            
        case MCC_STD_GNU89:
            init_c89_features(features);
            init_gnu_features(features);
            break;
            
        case MCC_STD_GNU99:
            init_c99_features(features);
            init_gnu_features(features);
            break;
            
        case MCC_STD_GNU11:
            init_c11_features(features);
            init_gnu_features(features);
            break;
            
        default:
            init_c89_features(features);
            break;
    }
}

bool mcc_c_std_has_feature(mcc_c_std_t std, mcc_feature_id_t feature)
{
    mcc_c_features_t features;
    mcc_c_std_init_features(std, &features);
    return MCC_FEATURES_HAS(features, feature);
}

mcc_c_std_t mcc_c_std_get_base(mcc_c_std_t std)
{
    const mcc_c_std_info_t *info = mcc_c_std_get_info(std);
    return info ? info->base_std : MCC_STD_C89;
}

bool mcc_c_std_is_gnu(mcc_c_std_t std)
{
    const mcc_c_std_info_t *info = mcc_c_std_get_info(std);
    return info ? info->is_gnu : false;
}

int mcc_c_std_compare(mcc_c_std_t a, mcc_c_std_t b)
{
    /* Resolve to base standards for comparison */
    mcc_c_std_t base_a = mcc_c_std_get_base(mcc_c_std_resolve(a));
    mcc_c_std_t base_b = mcc_c_std_get_base(mcc_c_std_resolve(b));
    
    const mcc_c_std_info_t *info_a = mcc_c_std_get_info(base_a);
    const mcc_c_std_info_t *info_b = mcc_c_std_get_info(base_b);
    
    if (!info_a || !info_b) {
        return 0;
    }
    
    if (info_a->year < info_b->year) return -1;
    if (info_a->year > info_b->year) return 1;
    return 0;
}

const mcc_predefined_macro_t *mcc_c_std_get_predefined_macros(mcc_c_std_t std, size_t *count)
{
    const mcc_predefined_macro_t *macros = NULL;
    
    std = mcc_c_std_resolve(std);
    
    switch (std) {
        case MCC_STD_C89:
        case MCC_STD_C90:
            macros = c89_predefined_macros;
            break;
        case MCC_STD_C99:
            macros = c99_predefined_macros;
            break;
        case MCC_STD_C11:
            macros = c11_predefined_macros;
            break;
        case MCC_STD_C17:
            macros = c17_predefined_macros;
            break;
        case MCC_STD_C23:
            macros = c23_predefined_macros;
            break;
        case MCC_STD_GNU89:
            macros = gnu89_predefined_macros;
            break;
        case MCC_STD_GNU99:
        case MCC_STD_GNU11:
            macros = gnu99_predefined_macros;
            break;
        default:
            macros = c89_predefined_macros;
            break;
    }
    
    if (count) {
        *count = 0;
        for (const mcc_predefined_macro_t *m = macros; m->name != NULL; m++) {
            (*count)++;
        }
    }
    
    return macros;
}
