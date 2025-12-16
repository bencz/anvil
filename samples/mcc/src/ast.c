/*
 * MCC - Micro C Compiler
 * AST utilities
 */

#include "mcc.h"

static const char *ast_kind_names[] = {
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
};

static const char *binop_names[] = {
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

static const char *unop_names[] = {
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

mcc_ast_node_t *mcc_ast_create(mcc_context_t *ctx, mcc_ast_kind_t kind, mcc_location_t loc)
{
    mcc_ast_node_t *node = mcc_alloc(ctx, sizeof(mcc_ast_node_t));
    node->kind = kind;
    node->location = loc;
    return node;
}

const char *mcc_ast_kind_name(mcc_ast_kind_t kind)
{
    if (kind < AST_NODE_COUNT && ast_kind_names[kind]) {
        return ast_kind_names[kind];
    }
    return "Unknown";
}

const char *mcc_binop_name(mcc_binop_t op)
{
    if (op < BINOP_COUNT) {
        return binop_names[op];
    }
    return "?";
}

const char *mcc_unop_name(mcc_unop_t op)
{
    if (op < UNOP_COUNT) {
        return unop_names[op];
    }
    return "?";
}

static void print_indent(FILE *out, int indent)
{
    for (int i = 0; i < indent; i++) {
        fprintf(out, "  ");
    }
}

void mcc_ast_print(mcc_ast_node_t *node, int indent)
{
    mcc_ast_dump(node, stdout);
}

static void ast_dump_node(mcc_ast_node_t *node, FILE *out, int indent)
{
    if (!node) {
        print_indent(out, indent);
        fprintf(out, "(null)\n");
        return;
    }
    
    print_indent(out, indent);
    fprintf(out, "%s", mcc_ast_kind_name(node->kind));
    
    if (node->location.filename) {
        fprintf(out, " <%s:%d:%d>",
                node->location.filename, node->location.line, node->location.column);
    }
    
    switch (node->kind) {
        case AST_FUNC_DECL:
            fprintf(out, " '%s'", node->data.func_decl.name);
            if (node->data.func_decl.is_static) fprintf(out, " static");
            break;
            
        case AST_VAR_DECL:
            fprintf(out, " '%s'", node->data.var_decl.name);
            if (node->data.var_decl.is_static) fprintf(out, " static");
            if (node->data.var_decl.is_extern) fprintf(out, " extern");
            break;
            
        case AST_PARAM_DECL:
            if (node->data.param_decl.name) {
                fprintf(out, " '%s'", node->data.param_decl.name);
            }
            break;
            
        case AST_IDENT_EXPR:
            fprintf(out, " '%s'", node->data.ident_expr.name);
            break;
            
        case AST_INT_LIT:
            fprintf(out, " %llu", (unsigned long long)node->data.int_lit.value);
            break;
            
        case AST_FLOAT_LIT:
            fprintf(out, " %g", node->data.float_lit.value);
            break;
            
        case AST_CHAR_LIT:
            fprintf(out, " '%c'", node->data.char_lit.value);
            break;
            
        case AST_STRING_LIT:
            fprintf(out, " \"%s\"", node->data.string_lit.value);
            break;
            
        case AST_BINARY_EXPR:
            fprintf(out, " '%s'", mcc_binop_name(node->data.binary_expr.op));
            break;
            
        case AST_UNARY_EXPR:
            fprintf(out, " '%s'", mcc_unop_name(node->data.unary_expr.op));
            break;
            
        case AST_MEMBER_EXPR:
            fprintf(out, " %s '%s'",
                    node->data.member_expr.is_arrow ? "->" : ".",
                    node->data.member_expr.member);
            break;
            
        case AST_GOTO_STMT:
            fprintf(out, " '%s'", node->data.goto_stmt.label);
            break;
            
        case AST_LABEL_STMT:
            fprintf(out, " '%s'", node->data.label_stmt.label);
            break;
            
        default:
            break;
    }
    
    fprintf(out, "\n");
    
    /* Print children */
    switch (node->kind) {
        case AST_TRANSLATION_UNIT:
            for (size_t i = 0; i < node->data.translation_unit.num_decls; i++) {
                ast_dump_node(node->data.translation_unit.decls[i], out, indent + 1);
            }
            break;
            
        case AST_FUNC_DECL:
            for (size_t i = 0; i < node->data.func_decl.num_params; i++) {
                ast_dump_node(node->data.func_decl.params[i], out, indent + 1);
            }
            if (node->data.func_decl.body) {
                ast_dump_node(node->data.func_decl.body, out, indent + 1);
            }
            break;
            
        case AST_VAR_DECL:
            if (node->data.var_decl.init) {
                ast_dump_node(node->data.var_decl.init, out, indent + 1);
            }
            break;
            
        case AST_COMPOUND_STMT:
            for (size_t i = 0; i < node->data.compound_stmt.num_stmts; i++) {
                ast_dump_node(node->data.compound_stmt.stmts[i], out, indent + 1);
            }
            break;
            
        case AST_EXPR_STMT:
            if (node->data.expr_stmt.expr) {
                ast_dump_node(node->data.expr_stmt.expr, out, indent + 1);
            }
            break;
            
        case AST_IF_STMT:
            ast_dump_node(node->data.if_stmt.cond, out, indent + 1);
            ast_dump_node(node->data.if_stmt.then_stmt, out, indent + 1);
            if (node->data.if_stmt.else_stmt) {
                ast_dump_node(node->data.if_stmt.else_stmt, out, indent + 1);
            }
            break;
            
        case AST_WHILE_STMT:
            ast_dump_node(node->data.while_stmt.cond, out, indent + 1);
            ast_dump_node(node->data.while_stmt.body, out, indent + 1);
            break;
            
        case AST_DO_STMT:
            ast_dump_node(node->data.do_stmt.body, out, indent + 1);
            ast_dump_node(node->data.do_stmt.cond, out, indent + 1);
            break;
            
        case AST_FOR_STMT:
            if (node->data.for_stmt.init_decl) {
                ast_dump_node(node->data.for_stmt.init_decl, out, indent + 1);
            }
            if (node->data.for_stmt.init) {
                ast_dump_node(node->data.for_stmt.init, out, indent + 1);
            }
            if (node->data.for_stmt.cond) {
                ast_dump_node(node->data.for_stmt.cond, out, indent + 1);
            }
            if (node->data.for_stmt.incr) {
                ast_dump_node(node->data.for_stmt.incr, out, indent + 1);
            }
            ast_dump_node(node->data.for_stmt.body, out, indent + 1);
            break;
            
        case AST_SWITCH_STMT:
            ast_dump_node(node->data.switch_stmt.expr, out, indent + 1);
            ast_dump_node(node->data.switch_stmt.body, out, indent + 1);
            break;
            
        case AST_CASE_STMT:
            ast_dump_node(node->data.case_stmt.expr, out, indent + 1);
            ast_dump_node(node->data.case_stmt.stmt, out, indent + 1);
            break;
            
        case AST_DEFAULT_STMT:
            ast_dump_node(node->data.default_stmt.stmt, out, indent + 1);
            break;
            
        case AST_RETURN_STMT:
            if (node->data.return_stmt.expr) {
                ast_dump_node(node->data.return_stmt.expr, out, indent + 1);
            }
            break;
            
        case AST_LABEL_STMT:
            ast_dump_node(node->data.label_stmt.stmt, out, indent + 1);
            break;
            
        case AST_BINARY_EXPR:
            ast_dump_node(node->data.binary_expr.lhs, out, indent + 1);
            ast_dump_node(node->data.binary_expr.rhs, out, indent + 1);
            break;
            
        case AST_UNARY_EXPR:
            ast_dump_node(node->data.unary_expr.operand, out, indent + 1);
            break;
            
        case AST_TERNARY_EXPR:
            ast_dump_node(node->data.ternary_expr.cond, out, indent + 1);
            ast_dump_node(node->data.ternary_expr.then_expr, out, indent + 1);
            ast_dump_node(node->data.ternary_expr.else_expr, out, indent + 1);
            break;
            
        case AST_CALL_EXPR:
            ast_dump_node(node->data.call_expr.func, out, indent + 1);
            for (size_t i = 0; i < node->data.call_expr.num_args; i++) {
                ast_dump_node(node->data.call_expr.args[i], out, indent + 1);
            }
            break;
            
        case AST_SUBSCRIPT_EXPR:
            ast_dump_node(node->data.subscript_expr.array, out, indent + 1);
            ast_dump_node(node->data.subscript_expr.index, out, indent + 1);
            break;
            
        case AST_MEMBER_EXPR:
            ast_dump_node(node->data.member_expr.object, out, indent + 1);
            break;
            
        case AST_CAST_EXPR:
            ast_dump_node(node->data.cast_expr.expr, out, indent + 1);
            break;
            
        case AST_SIZEOF_EXPR:
            if (node->data.sizeof_expr.expr_arg) {
                ast_dump_node(node->data.sizeof_expr.expr_arg, out, indent + 1);
            }
            break;
            
        case AST_COMMA_EXPR:
            ast_dump_node(node->data.comma_expr.left, out, indent + 1);
            ast_dump_node(node->data.comma_expr.right, out, indent + 1);
            break;
            
        case AST_INIT_LIST:
            for (size_t i = 0; i < node->data.init_list.num_exprs; i++) {
                ast_dump_node(node->data.init_list.exprs[i], out, indent + 1);
            }
            break;
            
        default:
            break;
    }
}

void mcc_ast_dump(mcc_ast_node_t *node, FILE *out)
{
    ast_dump_node(node, out, 0);
}
