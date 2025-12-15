/*
 * MCC - Micro C Compiler
 * Token definitions
 */

#ifndef MCC_TOKEN_H
#define MCC_TOKEN_H

/* Token types */
typedef enum {
    /* End of file */
    TOK_EOF = 0,
    
    /* Identifiers and literals */
    TOK_IDENT,          /* identifier */
    TOK_INT_LIT,        /* integer literal */
    TOK_FLOAT_LIT,      /* floating-point literal */
    TOK_CHAR_LIT,       /* character literal */
    TOK_STRING_LIT,     /* string literal */
    
    /* Keywords - Storage class specifiers */
    TOK_AUTO,
    TOK_REGISTER,
    TOK_STATIC,
    TOK_EXTERN,
    TOK_TYPEDEF,
    
    /* Keywords - Type specifiers */
    TOK_VOID,
    TOK_CHAR,
    TOK_SHORT,
    TOK_INT,
    TOK_LONG,
    TOK_FLOAT,
    TOK_DOUBLE,
    TOK_SIGNED,
    TOK_UNSIGNED,
    TOK_STRUCT,
    TOK_UNION,
    TOK_ENUM,
    
    /* Keywords - Type qualifiers */
    TOK_CONST,
    TOK_VOLATILE,
    
    /* Keywords - Statements */
    TOK_IF,
    TOK_ELSE,
    TOK_SWITCH,
    TOK_CASE,
    TOK_DEFAULT,
    TOK_WHILE,
    TOK_DO,
    TOK_FOR,
    TOK_GOTO,
    TOK_CONTINUE,
    TOK_BREAK,
    TOK_RETURN,
    
    /* Keywords - Other */
    TOK_SIZEOF,
    
    /* Operators - Arithmetic */
    TOK_PLUS,           /* + */
    TOK_MINUS,          /* - */
    TOK_STAR,           /* * */
    TOK_SLASH,          /* / */
    TOK_PERCENT,        /* % */
    
    /* Operators - Comparison */
    TOK_EQ,             /* == */
    TOK_NE,             /* != */
    TOK_LT,             /* < */
    TOK_GT,             /* > */
    TOK_LE,             /* <= */
    TOK_GE,             /* >= */
    
    /* Operators - Logical */
    TOK_AND,            /* && */
    TOK_OR,             /* || */
    TOK_NOT,            /* ! */
    
    /* Operators - Bitwise */
    TOK_AMP,            /* & */
    TOK_PIPE,           /* | */
    TOK_CARET,          /* ^ */
    TOK_TILDE,          /* ~ */
    TOK_LSHIFT,         /* << */
    TOK_RSHIFT,         /* >> */
    
    /* Operators - Assignment */
    TOK_ASSIGN,         /* = */
    TOK_PLUS_ASSIGN,    /* += */
    TOK_MINUS_ASSIGN,   /* -= */
    TOK_STAR_ASSIGN,    /* *= */
    TOK_SLASH_ASSIGN,   /* /= */
    TOK_PERCENT_ASSIGN, /* %= */
    TOK_AMP_ASSIGN,     /* &= */
    TOK_PIPE_ASSIGN,    /* |= */
    TOK_CARET_ASSIGN,   /* ^= */
    TOK_LSHIFT_ASSIGN,  /* <<= */
    TOK_RSHIFT_ASSIGN,  /* >>= */
    
    /* Operators - Increment/Decrement */
    TOK_INC,            /* ++ */
    TOK_DEC,            /* -- */
    
    /* Operators - Other */
    TOK_ARROW,          /* -> */
    TOK_DOT,            /* . */
    TOK_QUESTION,       /* ? */
    TOK_COLON,          /* : */
    TOK_COMMA,          /* , */
    TOK_SEMICOLON,      /* ; */
    
    /* Delimiters */
    TOK_LPAREN,         /* ( */
    TOK_RPAREN,         /* ) */
    TOK_LBRACKET,       /* [ */
    TOK_RBRACKET,       /* ] */
    TOK_LBRACE,         /* { */
    TOK_RBRACE,         /* } */
    
    /* Preprocessor (after preprocessing, these shouldn't appear) */
    TOK_HASH,           /* # */
    TOK_HASH_HASH,      /* ## */
    
    /* Special */
    TOK_ELLIPSIS,       /* ... */
    TOK_NEWLINE,        /* Used by preprocessor */
    
    TOK_COUNT
} mcc_token_type_t;

/* Integer literal suffix */
typedef enum {
    INT_SUFFIX_NONE = 0,
    INT_SUFFIX_U    = 1,    /* unsigned */
    INT_SUFFIX_L    = 2,    /* long */
    INT_SUFFIX_UL   = 3,    /* unsigned long */
    INT_SUFFIX_LL   = 4,    /* long long (C99, but we track it) */
    INT_SUFFIX_ULL  = 5     /* unsigned long long */
} mcc_int_suffix_t;

/* Float literal suffix */
typedef enum {
    FLOAT_SUFFIX_NONE = 0,  /* double */
    FLOAT_SUFFIX_F,         /* float */
    FLOAT_SUFFIX_L          /* long double */
} mcc_float_suffix_t;

/* Token structure */
typedef struct mcc_token {
    mcc_token_type_t type;
    mcc_location_t location;
    
    /* Token text (for identifiers, literals) */
    const char *text;
    size_t text_len;
    
    /* Literal values */
    union {
        struct {
            uint64_t value;
            mcc_int_suffix_t suffix;
        } int_val;
        
        struct {
            double value;
            mcc_float_suffix_t suffix;
        } float_val;
        
        struct {
            int value;  /* Character code */
        } char_val;
        
        struct {
            const char *value;
            size_t length;
        } string_val;
    } literal;
    
    /* For preprocessor: is at beginning of line? */
    bool at_bol;
    
    /* Has whitespace before this token? */
    bool has_space;
    
    /* Next token in list (for preprocessor) */
    struct mcc_token *next;
} mcc_token_t;

/* Token utilities */
const char *mcc_token_type_name(mcc_token_type_t type);
const char *mcc_token_to_string(mcc_token_t *tok);
bool mcc_token_is_keyword(mcc_token_type_t type);
bool mcc_token_is_type_specifier(mcc_token_type_t type);
bool mcc_token_is_type_qualifier(mcc_token_type_t type);
bool mcc_token_is_storage_class(mcc_token_type_t type);
bool mcc_token_is_assignment_op(mcc_token_type_t type);
bool mcc_token_is_comparison_op(mcc_token_type_t type);
bool mcc_token_is_unary_op(mcc_token_type_t type);

#endif /* MCC_TOKEN_H */
