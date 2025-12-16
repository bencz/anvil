/*
 * MCC - Micro C Compiler
 * AST (Abstract Syntax Tree) definitions
 */

#ifndef MCC_AST_H
#define MCC_AST_H

/* Forward declarations */
typedef struct mcc_ast_node mcc_ast_node_t;
struct mcc_type;

/* Generic association for _Generic (C11) */
typedef struct mcc_generic_assoc {
    struct mcc_type *type;          /* NULL for default association */
    mcc_ast_node_t *expr;
    struct mcc_generic_assoc *next;
} mcc_generic_assoc_t;

/* AST node kinds */
typedef enum {
    /* Translation unit */
    AST_TRANSLATION_UNIT,
    
    /* Declarations */
    AST_FUNC_DECL,          /* Function declaration/definition */
    AST_VAR_DECL,           /* Variable declaration */
    AST_PARAM_DECL,         /* Parameter declaration */
    AST_TYPEDEF_DECL,       /* Typedef declaration */
    AST_STRUCT_DECL,        /* Struct declaration */
    AST_UNION_DECL,         /* Union declaration */
    AST_ENUM_DECL,          /* Enum declaration */
    AST_ENUMERATOR,         /* Enum constant */
    AST_FIELD_DECL,         /* Struct/union field */
    
    /* Statements */
    AST_COMPOUND_STMT,      /* { ... } */
    AST_EXPR_STMT,          /* expression; */
    AST_IF_STMT,            /* if (cond) then [else] */
    AST_SWITCH_STMT,        /* switch (expr) { ... } */
    AST_CASE_STMT,          /* case expr: */
    AST_DEFAULT_STMT,       /* default: */
    AST_WHILE_STMT,         /* while (cond) body */
    AST_DO_STMT,            /* do body while (cond) */
    AST_FOR_STMT,           /* for (init; cond; incr) body */
    AST_GOTO_STMT,          /* goto label; */
    AST_CONTINUE_STMT,      /* continue; */
    AST_BREAK_STMT,         /* break; */
    AST_RETURN_STMT,        /* return [expr]; */
    AST_LABEL_STMT,         /* label: stmt */
    AST_NULL_STMT,          /* ; (empty statement) */
    
    /* Expressions */
    AST_IDENT_EXPR,         /* identifier */
    AST_INT_LIT,            /* integer literal */
    AST_FLOAT_LIT,          /* float literal */
    AST_CHAR_LIT,           /* character literal */
    AST_STRING_LIT,         /* string literal */
    AST_BINARY_EXPR,        /* lhs op rhs */
    AST_UNARY_EXPR,         /* op operand */
    AST_POSTFIX_EXPR,       /* operand op */
    AST_TERNARY_EXPR,       /* cond ? then : else */
    AST_CALL_EXPR,          /* func(args) */
    AST_SUBSCRIPT_EXPR,     /* array[index] */
    AST_MEMBER_EXPR,        /* struct.member or struct->member */
    AST_CAST_EXPR,          /* (type)expr */
    AST_SIZEOF_EXPR,        /* sizeof(type) or sizeof expr */
    AST_COMMA_EXPR,         /* expr1, expr2 */
    AST_INIT_LIST,          /* { expr1, expr2, ... } */
    
    /* C99+ features */
    AST_COMPOUND_LIT,       /* (type){ init } - C99 compound literal */
    AST_DESIGNATED_INIT,    /* .field = value or [index] = value */
    AST_FIELD_DESIGNATOR,   /* .field in designated initializer */
    AST_INDEX_DESIGNATOR,   /* [index] in designated initializer */
    AST_ALIGNOF_EXPR,       /* _Alignof(type) - C11 */
    AST_STATIC_ASSERT,      /* _Static_assert(expr, msg) - C11 */
    AST_GENERIC_EXPR,       /* _Generic(expr, ...) - C11 */
    
    /* C23 features */
    AST_NULL_PTR,           /* nullptr - C23 */
    
    /* GNU extensions */
    AST_STMT_EXPR,          /* ({ ... }) - GNU statement expression */
    AST_LABEL_ADDR,         /* &&label - GNU labels as values */
    AST_GOTO_EXPR,          /* goto *expr - GNU computed goto */
    
    AST_NODE_COUNT
} mcc_ast_kind_t;

/* Binary operators */
typedef enum {
    /* Arithmetic */
    BINOP_ADD,              /* + */
    BINOP_SUB,              /* - */
    BINOP_MUL,              /* * */
    BINOP_DIV,              /* / */
    BINOP_MOD,              /* % */
    
    /* Comparison */
    BINOP_EQ,               /* == */
    BINOP_NE,               /* != */
    BINOP_LT,               /* < */
    BINOP_GT,               /* > */
    BINOP_LE,               /* <= */
    BINOP_GE,               /* >= */
    
    /* Logical */
    BINOP_AND,              /* && */
    BINOP_OR,               /* || */
    
    /* Bitwise */
    BINOP_BIT_AND,          /* & */
    BINOP_BIT_OR,           /* | */
    BINOP_BIT_XOR,          /* ^ */
    BINOP_LSHIFT,           /* << */
    BINOP_RSHIFT,           /* >> */
    
    /* Assignment */
    BINOP_ASSIGN,           /* = */
    BINOP_ADD_ASSIGN,       /* += */
    BINOP_SUB_ASSIGN,       /* -= */
    BINOP_MUL_ASSIGN,       /* *= */
    BINOP_DIV_ASSIGN,       /* /= */
    BINOP_MOD_ASSIGN,       /* %= */
    BINOP_AND_ASSIGN,       /* &= */
    BINOP_OR_ASSIGN,        /* |= */
    BINOP_XOR_ASSIGN,       /* ^= */
    BINOP_LSHIFT_ASSIGN,    /* <<= */
    BINOP_RSHIFT_ASSIGN,    /* >>= */
    
    BINOP_COUNT
} mcc_binop_t;

/* Unary operators */
typedef enum {
    UNOP_NEG,               /* - (negation) */
    UNOP_POS,               /* + (positive) */
    UNOP_NOT,               /* ! (logical not) */
    UNOP_BIT_NOT,           /* ~ (bitwise not) */
    UNOP_DEREF,             /* * (dereference) */
    UNOP_ADDR,              /* & (address-of) */
    UNOP_PRE_INC,           /* ++x */
    UNOP_PRE_DEC,           /* --x */
    UNOP_POST_INC,          /* x++ */
    UNOP_POST_DEC,          /* x-- */
    
    UNOP_COUNT
} mcc_unop_t;

/* AST node structure */
struct mcc_ast_node {
    mcc_ast_kind_t kind;
    mcc_location_t location;
    struct mcc_type *type;           /* Resolved type (set by sema) */
    
    union {
        /* Translation unit */
        struct {
            mcc_ast_node_t **decls;
            size_t num_decls;
        } translation_unit;
        
        /* Function declaration */
        struct {
            const char *name;
            struct mcc_type *func_type;
            mcc_ast_node_t **params;
            size_t num_params;
            mcc_ast_node_t *body;   /* NULL for prototype */
            bool is_definition;
            bool is_static;
            bool is_variadic;       /* Has ... parameter */
            bool is_inline;         /* C99 inline */
            bool is_noreturn;       /* C11 _Noreturn */
        } func_decl;
        
        /* Variable declaration */
        struct {
            const char *name;
            struct mcc_type *var_type;
            mcc_ast_node_t *init;   /* NULL if no initializer */
            bool is_static;
            bool is_extern;
            bool is_const;
            bool is_volatile;
        } var_decl;
        
        /* Parameter declaration */
        struct {
            const char *name;       /* Can be NULL in prototypes */
            struct mcc_type *param_type;
        } param_decl;
        
        /* Typedef declaration */
        struct {
            const char *name;
            struct mcc_type *type;
        } typedef_decl;
        
        /* Struct/Union declaration */
        struct {
            const char *tag;        /* NULL for anonymous */
            mcc_ast_node_t **fields;
            size_t num_fields;
            bool is_definition;
        } struct_decl;
        
        /* Enum declaration */
        struct {
            const char *tag;
            mcc_ast_node_t **enumerators;
            size_t num_enumerators;
            bool is_definition;
        } enum_decl;
        
        /* Enumerator */
        struct {
            const char *name;
            mcc_ast_node_t *value;  /* NULL for auto value */
            int resolved_value;     /* Set by sema */
        } enumerator;
        
        /* Field declaration */
        struct {
            const char *name;
            struct mcc_type *field_type;
            mcc_ast_node_t *bitfield; /* NULL if not bitfield */
        } field_decl;
        
        /* Compound statement */
        struct {
            mcc_ast_node_t **stmts;
            size_t num_stmts;
        } compound_stmt;
        
        /* Expression statement */
        struct {
            mcc_ast_node_t *expr;   /* NULL for empty stmt */
        } expr_stmt;
        
        /* If statement */
        struct {
            mcc_ast_node_t *cond;
            mcc_ast_node_t *then_stmt;
            mcc_ast_node_t *else_stmt; /* NULL if no else */
        } if_stmt;
        
        /* Switch statement */
        struct {
            mcc_ast_node_t *expr;
            mcc_ast_node_t *body;
        } switch_stmt;
        
        /* Case statement */
        struct {
            mcc_ast_node_t *expr;
            mcc_ast_node_t *end_expr;   /* For GNU case ranges (case 1 ... 5:) */
            mcc_ast_node_t *stmt;
        } case_stmt;
        
        /* Default statement */
        struct {
            mcc_ast_node_t *stmt;
        } default_stmt;
        
        /* While statement */
        struct {
            mcc_ast_node_t *cond;
            mcc_ast_node_t *body;
        } while_stmt;
        
        /* Do-while statement */
        struct {
            mcc_ast_node_t *body;
            mcc_ast_node_t *cond;
        } do_stmt;
        
        /* For statement */
        struct {
            mcc_ast_node_t *init;       /* Expression init (C89) */
            mcc_ast_node_t *init_decl;  /* Declaration init (C99) */
            mcc_ast_node_t *cond;
            mcc_ast_node_t *incr;
            mcc_ast_node_t *body;
        } for_stmt;
        
        /* Goto statement */
        struct {
            const char *label;
        } goto_stmt;
        
        /* Return statement */
        struct {
            mcc_ast_node_t *expr;   /* NULL for void return */
        } return_stmt;
        
        /* Label statement */
        struct {
            const char *label;
            mcc_ast_node_t *stmt;
        } label_stmt;
        
        /* Identifier expression */
        struct {
            const char *name;
            void *symbol;           /* Resolved symbol (set by sema) */
        } ident_expr;
        
        /* Integer literal */
        struct {
            uint64_t value;
            mcc_int_suffix_t suffix;
        } int_lit;
        
        /* Float literal */
        struct {
            double value;
            mcc_float_suffix_t suffix;
        } float_lit;
        
        /* Character literal */
        struct {
            int value;
        } char_lit;
        
        /* String literal */
        struct {
            const char *value;
            size_t length;
        } string_lit;
        
        /* Binary expression */
        struct {
            mcc_binop_t op;
            mcc_ast_node_t *lhs;
            mcc_ast_node_t *rhs;
        } binary_expr;
        
        /* Unary expression */
        struct {
            mcc_unop_t op;
            mcc_ast_node_t *operand;
        } unary_expr;
        
        /* Ternary expression */
        struct {
            mcc_ast_node_t *cond;
            mcc_ast_node_t *then_expr;
            mcc_ast_node_t *else_expr;
        } ternary_expr;
        
        /* Call expression */
        struct {
            mcc_ast_node_t *func;
            mcc_ast_node_t **args;
            size_t num_args;
        } call_expr;
        
        /* Subscript expression */
        struct {
            mcc_ast_node_t *array;
            mcc_ast_node_t *index;
        } subscript_expr;
        
        /* Member expression */
        struct {
            mcc_ast_node_t *object;
            const char *member;
            bool is_arrow;          /* true for ->, false for . */
        } member_expr;
        
        /* Cast expression */
        struct {
            struct mcc_type *target_type;
            mcc_ast_node_t *expr;
        } cast_expr;
        
        /* Sizeof expression */
        struct {
            struct mcc_type *type_arg;   /* For sizeof(type) */
            mcc_ast_node_t *expr_arg; /* For sizeof expr */
        } sizeof_expr;
        
        /* Comma expression */
        struct {
            mcc_ast_node_t *left;
            mcc_ast_node_t *right;
        } comma_expr;
        
        /* Initializer list */
        struct {
            mcc_ast_node_t **exprs;
            size_t num_exprs;
        } init_list;
        
        /* C99: Compound literal (type){ init } */
        struct {
            struct mcc_type *type;
            mcc_ast_node_t *init;
        } compound_lit;
        
        /* C99: Designated initializer */
        struct {
            mcc_ast_node_t *designator;
            mcc_ast_node_t *value;
        } designated_init;
        
        /* C99: Field designator .field */
        struct {
            const char *name;
            mcc_ast_node_t *next;   /* Next designator in chain */
        } field_designator;
        
        /* C99: Index designator [index] */
        struct {
            mcc_ast_node_t *index;
            mcc_ast_node_t *next;   /* Next designator in chain */
        } index_designator;
        
        /* C11: _Alignof expression */
        struct {
            struct mcc_type *type_arg;
        } alignof_expr;
        
        /* C11: _Static_assert */
        struct {
            mcc_ast_node_t *expr;
            const char *message;
        } static_assert;
        
        /* C11: _Generic selection */
        struct {
            mcc_ast_node_t *controlling_expr;
            struct mcc_generic_assoc *associations;
            int num_associations;
            mcc_ast_node_t *default_expr;
        } generic_expr;
        
        /* GNU: Statement expression ({ ... }) */
        struct {
            mcc_ast_node_t *stmt;
        } stmt_expr;
        
        /* GNU: Label address &&label */
        struct {
            const char *label;
        } label_addr;
        
        /* GNU: Computed goto (goto *expr) */
        struct {
            mcc_ast_node_t *expr;
        } goto_expr;
    } data;
};

/* AST creation functions */
mcc_ast_node_t *mcc_ast_create(mcc_context_t *ctx, mcc_ast_kind_t kind, mcc_location_t loc);

/* AST utilities */
const char *mcc_ast_kind_name(mcc_ast_kind_t kind);
const char *mcc_binop_name(mcc_binop_t op);
const char *mcc_unop_name(mcc_unop_t op);

/* AST printing (for debugging) */
void mcc_ast_print(mcc_ast_node_t *node, int indent);
void mcc_ast_dump(mcc_ast_node_t *node, FILE *out);

#endif /* MCC_AST_H */
