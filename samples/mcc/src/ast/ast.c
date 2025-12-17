/*
 * MCC - Micro C Compiler
 * AST core utilities
 */

#include "ast_internal.h"

/* AST node kind names */
const char *mcc_ast_kind_names[] = {
    [AST_TRANSLATION_UNIT] = "TranslationUnit",
    [AST_FUNC_DECL]        = "FunctionDecl",
    [AST_VAR_DECL]         = "VarDecl",
    [AST_PARAM_DECL]       = "ParamDecl",
    [AST_TYPEDEF_DECL]     = "TypedefDecl",
    [AST_STRUCT_DECL]      = "StructDecl",
    [AST_UNION_DECL]       = "UnionDecl",
    [AST_ENUM_DECL]        = "EnumDecl",
    [AST_ENUMERATOR]       = "Enumerator",
    [AST_FIELD_DECL]       = "FieldDecl",
    [AST_COMPOUND_STMT]    = "CompoundStmt",
    [AST_EXPR_STMT]        = "ExprStmt",
    [AST_IF_STMT]          = "IfStmt",
    [AST_SWITCH_STMT]      = "SwitchStmt",
    [AST_CASE_STMT]        = "CaseStmt",
    [AST_DEFAULT_STMT]     = "DefaultStmt",
    [AST_WHILE_STMT]       = "WhileStmt",
    [AST_DO_STMT]          = "DoStmt",
    [AST_FOR_STMT]         = "ForStmt",
    [AST_GOTO_STMT]        = "GotoStmt",
    [AST_CONTINUE_STMT]    = "ContinueStmt",
    [AST_BREAK_STMT]       = "BreakStmt",
    [AST_RETURN_STMT]      = "ReturnStmt",
    [AST_LABEL_STMT]       = "LabelStmt",
    [AST_NULL_STMT]        = "NullStmt",
    [AST_IDENT_EXPR]       = "IdentExpr",
    [AST_INT_LIT]          = "IntLit",
    [AST_FLOAT_LIT]        = "FloatLit",
    [AST_CHAR_LIT]         = "CharLit",
    [AST_STRING_LIT]       = "StringLit",
    [AST_BINARY_EXPR]      = "BinaryExpr",
    [AST_UNARY_EXPR]       = "UnaryExpr",
    [AST_POSTFIX_EXPR]     = "PostfixExpr",
    [AST_TERNARY_EXPR]     = "TernaryExpr",
    [AST_CALL_EXPR]        = "CallExpr",
    [AST_SUBSCRIPT_EXPR]   = "SubscriptExpr",
    [AST_MEMBER_EXPR]      = "MemberExpr",
    [AST_CAST_EXPR]        = "CastExpr",
    [AST_SIZEOF_EXPR]      = "SizeofExpr",
    [AST_COMMA_EXPR]       = "CommaExpr",
    [AST_INIT_LIST]        = "InitList",
    [AST_DECL_LIST]        = "DeclList",
};

/* Binary operator names */
const char *mcc_binop_names[] = {
    [BINOP_ADD]           = "+",
    [BINOP_SUB]           = "-",
    [BINOP_MUL]           = "*",
    [BINOP_DIV]           = "/",
    [BINOP_MOD]           = "%",
    [BINOP_EQ]            = "==",
    [BINOP_NE]            = "!=",
    [BINOP_LT]            = "<",
    [BINOP_GT]            = ">",
    [BINOP_LE]            = "<=",
    [BINOP_GE]            = ">=",
    [BINOP_AND]           = "&&",
    [BINOP_OR]            = "||",
    [BINOP_BIT_AND]       = "&",
    [BINOP_BIT_OR]        = "|",
    [BINOP_BIT_XOR]       = "^",
    [BINOP_LSHIFT]        = "<<",
    [BINOP_RSHIFT]        = ">>",
    [BINOP_ASSIGN]        = "=",
    [BINOP_ADD_ASSIGN]    = "+=",
    [BINOP_SUB_ASSIGN]    = "-=",
    [BINOP_MUL_ASSIGN]    = "*=",
    [BINOP_DIV_ASSIGN]    = "/=",
    [BINOP_MOD_ASSIGN]    = "%=",
    [BINOP_AND_ASSIGN]    = "&=",
    [BINOP_OR_ASSIGN]     = "|=",
    [BINOP_XOR_ASSIGN]    = "^=",
    [BINOP_LSHIFT_ASSIGN] = "<<=",
    [BINOP_RSHIFT_ASSIGN] = ">>=",
};

/* Unary operator names */
const char *mcc_unop_names[] = {
    [UNOP_NEG]      = "-",
    [UNOP_POS]      = "+",
    [UNOP_NOT]      = "!",
    [UNOP_BIT_NOT]  = "~",
    [UNOP_DEREF]    = "*",
    [UNOP_ADDR]     = "&",
    [UNOP_PRE_INC]  = "++",
    [UNOP_PRE_DEC]  = "--",
    [UNOP_POST_INC] = "++",
    [UNOP_POST_DEC] = "--",
};

/* Integer literal suffix names */
const char *mcc_int_suffix_names[] = {
    [INT_SUFFIX_NONE] = "",
    [INT_SUFFIX_U]    = "U",
    [INT_SUFFIX_L]    = "L",
    [INT_SUFFIX_UL]   = "UL",
    [INT_SUFFIX_LL]   = "LL",
    [INT_SUFFIX_ULL]  = "ULL",
};

/* Float literal suffix names */
const char *mcc_float_suffix_names[] = {
    [FLOAT_SUFFIX_NONE] = "",
    [FLOAT_SUFFIX_F]    = "F",
    [FLOAT_SUFFIX_L]    = "L",
};

/* Attribute kind names */
const char *mcc_attr_kind_names[] = {
    [MCC_ATTR_NONE]         = "none",
    [MCC_ATTR_DEPRECATED]   = "deprecated",
    [MCC_ATTR_FALLTHROUGH]  = "fallthrough",
    [MCC_ATTR_NODISCARD]    = "nodiscard",
    [MCC_ATTR_MAYBE_UNUSED] = "maybe_unused",
    [MCC_ATTR_NORETURN]     = "noreturn",
    [MCC_ATTR_UNSEQUENCED]  = "unsequenced",
    [MCC_ATTR_REPRODUCIBLE] = "reproducible",
    [MCC_ATTR_GNU_PACKED]   = "packed",
    [MCC_ATTR_GNU_ALIGNED]  = "aligned",
    [MCC_ATTR_GNU_PURE]     = "pure",
    [MCC_ATTR_GNU_CONST]    = "const",
    [MCC_ATTR_GNU_UNUSED]   = "unused",
    [MCC_ATTR_UNKNOWN]      = "unknown",
};

/*
 * Create a new AST node
 */
mcc_ast_node_t *mcc_ast_create(mcc_context_t *ctx, mcc_ast_kind_t kind, mcc_location_t loc)
{
    mcc_ast_node_t *node = mcc_alloc(ctx, sizeof(mcc_ast_node_t));
    node->kind = kind;
    node->location = loc;
    return node;
}

/*
 * Get the name of an AST node kind
 */
const char *mcc_ast_kind_name(mcc_ast_kind_t kind)
{
    if (kind < AST_NODE_COUNT && mcc_ast_kind_names[kind]) {
        return mcc_ast_kind_names[kind];
    }
    return "Unknown";
}

/*
 * Get the name of a binary operator
 */
const char *mcc_binop_name(mcc_binop_t op)
{
    if (op < BINOP_COUNT) {
        return mcc_binop_names[op];
    }
    return "?";
}

/*
 * Get the name of a unary operator
 */
const char *mcc_unop_name(mcc_unop_t op)
{
    if (op < UNOP_COUNT) {
        return mcc_unop_names[op];
    }
    return "?";
}
