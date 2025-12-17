/*
 * MCC - Micro C Compiler
 * AST dump/print utilities
 */

#include "ast_internal.h"

/* Forward declarations */
static void ast_dump_node(mcc_ast_node_t *node, FILE *out, int indent);
static void dump_type(struct mcc_type *type, FILE *out);
static void dump_attributes(mcc_attribute_t *attrs, FILE *out);

/*
 * Print indentation (spaces only, no tree characters)
 */
static void print_indent(FILE *out, int indent)
{
    for (int i = 0; i < indent; i++) {
        fprintf(out, "  ");
    }
}

/*
 * Print type information
 */
static void dump_type(struct mcc_type *type, FILE *out)
{
    if (!type) {
        fprintf(out, "<no type>");
        return;
    }
    fprintf(out, "'%s'", mcc_type_to_string(type));
}

/*
 * Print C23/GNU attributes
 */
static void dump_attributes(mcc_attribute_t *attrs, FILE *out)
{
    if (!attrs) return;
    fprintf(out, " [[%s", mcc_attr_kind_names[attrs->kind]);
    if (attrs->message) {
        fprintf(out, "(\"%s\")", attrs->message);
    }
    for (mcc_attribute_t *a = attrs->next; a; a = a->next) {
        fprintf(out, ", %s", mcc_attr_kind_names[a->kind]);
        if (a->message) {
            fprintf(out, "(\"%s\")", a->message);
        }
    }
    fprintf(out, "]]");
}

/*
 * Print escaped string literal
 */
static void print_escaped_string(const char *str, size_t len, FILE *out)
{
    fputc('"', out);
    for (size_t i = 0; i < len && str[i]; i++) {
        unsigned char c = str[i];
        switch (c) {
            case '\n': fprintf(out, "\\n"); break;
            case '\t': fprintf(out, "\\t"); break;
            case '\r': fprintf(out, "\\r"); break;
            case '\\': fprintf(out, "\\\\"); break;
            case '"':  fprintf(out, "\\\""); break;
            case '\0': fprintf(out, "\\0"); break;
            default:
                if (c >= 32 && c < 127) {
                    fputc(c, out);
                } else {
                    fprintf(out, "\\x%02x", c);
                }
                break;
        }
    }
    fputc('"', out);
}

/*
 * Print character literal with escaping
 */
static void print_char_literal(int c, FILE *out)
{
    fputc('\'', out);
    switch (c) {
        case '\n': fprintf(out, "\\n"); break;
        case '\t': fprintf(out, "\\t"); break;
        case '\r': fprintf(out, "\\r"); break;
        case '\\': fprintf(out, "\\\\"); break;
        case '\'': fprintf(out, "\\'"); break;
        case '\0': fprintf(out, "\\0"); break;
        default:
            if (c >= 32 && c < 127) {
                fputc(c, out);
            } else {
                fprintf(out, "\\x%02x", (unsigned char)c);
            }
            break;
    }
    fputc('\'', out);
}

/*
 * Dump a single AST node and its children
 */
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
    
    /* Print node-specific details */
    switch (node->kind) {
        case AST_FUNC_DECL:
            fprintf(out, " '%s'", node->data.func_decl.name);
            if (node->data.func_decl.func_type) {
                fprintf(out, " ");
                dump_type(node->data.func_decl.func_type, out);
            }
            if (node->data.func_decl.is_definition) fprintf(out, " definition");
            if (node->data.func_decl.is_static) fprintf(out, " static");
            if (node->data.func_decl.is_inline) fprintf(out, " inline");
            if (node->data.func_decl.is_noreturn) fprintf(out, " _Noreturn");
            if (node->data.func_decl.is_variadic) fprintf(out, " variadic");
            dump_attributes(node->data.func_decl.attrs, out);
            break;
            
        case AST_VAR_DECL:
            fprintf(out, " '%s'", node->data.var_decl.name);
            if (node->data.var_decl.var_type) {
                fprintf(out, " ");
                dump_type(node->data.var_decl.var_type, out);
            }
            if (node->data.var_decl.is_static) fprintf(out, " static");
            if (node->data.var_decl.is_extern) fprintf(out, " extern");
            if (node->data.var_decl.is_const) fprintf(out, " const");
            if (node->data.var_decl.is_volatile) fprintf(out, " volatile");
            dump_attributes(node->data.var_decl.attrs, out);
            break;
            
        case AST_PARAM_DECL:
            if (node->data.param_decl.name) {
                fprintf(out, " '%s'", node->data.param_decl.name);
            }
            if (node->data.param_decl.param_type) {
                fprintf(out, " ");
                dump_type(node->data.param_decl.param_type, out);
            }
            break;
            
        case AST_TYPEDEF_DECL:
            fprintf(out, " '%s'", node->data.typedef_decl.name);
            if (node->data.typedef_decl.type) {
                fprintf(out, " -> ");
                dump_type(node->data.typedef_decl.type, out);
            }
            break;
            
        case AST_STRUCT_DECL:
            if (node->data.struct_decl.tag) {
                fprintf(out, " '%s'", node->data.struct_decl.tag);
            } else {
                fprintf(out, " (anonymous)");
            }
            if (node->data.struct_decl.is_definition) fprintf(out, " definition");
            break;
            
        case AST_UNION_DECL:
            if (node->data.struct_decl.tag) {
                fprintf(out, " '%s'", node->data.struct_decl.tag);
            } else {
                fprintf(out, " (anonymous)");
            }
            if (node->data.struct_decl.is_definition) fprintf(out, " definition");
            break;
            
        case AST_ENUM_DECL:
            if (node->data.enum_decl.tag) {
                fprintf(out, " '%s'", node->data.enum_decl.tag);
            } else {
                fprintf(out, " (anonymous)");
            }
            if (node->data.enum_decl.is_definition) fprintf(out, " definition");
            break;
            
        case AST_ENUMERATOR:
            fprintf(out, " '%s'", node->data.enumerator.name);
            fprintf(out, " = %d", node->data.enumerator.resolved_value);
            break;
            
        case AST_FIELD_DECL:
            if (node->data.field_decl.name) {
                fprintf(out, " '%s'", node->data.field_decl.name);
            } else {
                fprintf(out, " (anonymous)");
            }
            if (node->data.field_decl.field_type) {
                fprintf(out, " ");
                dump_type(node->data.field_decl.field_type, out);
            }
            if (node->data.field_decl.bitfield) {
                fprintf(out, " : bitfield");
            }
            break;
            
        case AST_IDENT_EXPR:
            fprintf(out, " '%s'", node->data.ident_expr.name);
            if (node->data.ident_expr.is_func_name) fprintf(out, " __func__");
            if (node->type) {
                fprintf(out, " ");
                dump_type(node->type, out);
            }
            break;
            
        case AST_INT_LIT:
            fprintf(out, " %llu%s",
                    (unsigned long long)node->data.int_lit.value,
                    mcc_int_suffix_names[node->data.int_lit.suffix]);
            if (node->type) {
                fprintf(out, " ");
                dump_type(node->type, out);
            }
            break;
            
        case AST_FLOAT_LIT:
            fprintf(out, " %g%s",
                    node->data.float_lit.value,
                    mcc_float_suffix_names[node->data.float_lit.suffix]);
            if (node->type) {
                fprintf(out, " ");
                dump_type(node->type, out);
            }
            break;
            
        case AST_CHAR_LIT:
            fprintf(out, " ");
            print_char_literal(node->data.char_lit.value, out);
            if (node->type) {
                fprintf(out, " ");
                dump_type(node->type, out);
            }
            break;
            
        case AST_STRING_LIT:
            fprintf(out, " ");
            print_escaped_string(node->data.string_lit.value, node->data.string_lit.length, out);
            fprintf(out, " (len=%zu)", node->data.string_lit.length);
            if (node->type) {
                fprintf(out, " ");
                dump_type(node->type, out);
            }
            break;
            
        case AST_BINARY_EXPR:
            fprintf(out, " '%s'", mcc_binop_name(node->data.binary_expr.op));
            if (node->type) {
                fprintf(out, " ");
                dump_type(node->type, out);
            }
            break;
            
        case AST_UNARY_EXPR:
            fprintf(out, " '%s'", mcc_unop_name(node->data.unary_expr.op));
            if (node->type) {
                fprintf(out, " ");
                dump_type(node->type, out);
            }
            break;
            
        case AST_POSTFIX_EXPR:
            fprintf(out, " '%s'", mcc_unop_name(node->data.unary_expr.op));
            if (node->type) {
                fprintf(out, " ");
                dump_type(node->type, out);
            }
            break;
            
        case AST_TERNARY_EXPR:
            if (node->type) {
                fprintf(out, " ");
                dump_type(node->type, out);
            }
            break;
            
        case AST_CALL_EXPR:
            if (node->type) {
                fprintf(out, " ");
                dump_type(node->type, out);
            }
            fprintf(out, " (args=%zu)", node->data.call_expr.num_args);
            break;
            
        case AST_SUBSCRIPT_EXPR:
            if (node->type) {
                fprintf(out, " ");
                dump_type(node->type, out);
            }
            break;
            
        case AST_MEMBER_EXPR:
            fprintf(out, " %s'%s'",
                    node->data.member_expr.is_arrow ? "->" : ".",
                    node->data.member_expr.member);
            if (node->type) {
                fprintf(out, " ");
                dump_type(node->type, out);
            }
            break;
            
        case AST_CAST_EXPR:
            if (node->data.cast_expr.target_type) {
                fprintf(out, " to ");
                dump_type(node->data.cast_expr.target_type, out);
            }
            break;
            
        case AST_SIZEOF_EXPR:
            if (node->data.sizeof_expr.type_arg) {
                fprintf(out, " sizeof(");
                dump_type(node->data.sizeof_expr.type_arg, out);
                fprintf(out, ")");
            } else {
                fprintf(out, " sizeof(expr)");
            }
            if (node->type) {
                fprintf(out, " ");
                dump_type(node->type, out);
            }
            break;
            
        case AST_ALIGNOF_EXPR:
            if (node->data.alignof_expr.type_arg) {
                fprintf(out, " _Alignof(");
                dump_type(node->data.alignof_expr.type_arg, out);
                fprintf(out, ")");
            } else {
                fprintf(out, " _Alignof(expr)");
            }
            break;
            
        case AST_COMPOUND_LIT:
            if (node->data.compound_lit.type) {
                fprintf(out, " ");
                dump_type(node->data.compound_lit.type, out);
            }
            break;
            
        case AST_STATIC_ASSERT:
            if (node->data.static_assert.message) {
                fprintf(out, " \"%s\"", node->data.static_assert.message);
            }
            break;
            
        case AST_GENERIC_EXPR:
            fprintf(out, " (%d associations)", node->data.generic_expr.num_associations);
            break;
            
        case AST_GOTO_STMT:
            fprintf(out, " '%s'", node->data.goto_stmt.label);
            break;
            
        case AST_LABEL_STMT:
            fprintf(out, " '%s':", node->data.label_stmt.label);
            break;
            
        case AST_LABEL_ADDR:
            fprintf(out, " &&'%s'", node->data.label_addr.label);
            break;
            
        case AST_NULL_PTR:
            fprintf(out, " nullptr");
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
            if (node->data.func_decl.num_params > 0) {
                print_indent(out, indent + 1);
                fprintf(out, "Parameters:\n");
                for (size_t i = 0; i < node->data.func_decl.num_params; i++) {
                    ast_dump_node(node->data.func_decl.params[i], out, indent + 2);
                }
            }
            if (node->data.func_decl.body) {
                print_indent(out, indent + 1);
                fprintf(out, "Body:\n");
                ast_dump_node(node->data.func_decl.body, out, indent + 2);
            }
            break;
            
        case AST_VAR_DECL:
            if (node->data.var_decl.init) {
                print_indent(out, indent + 1);
                fprintf(out, "Init:\n");
                ast_dump_node(node->data.var_decl.init, out, indent + 2);
            }
            break;
            
        case AST_DECL_LIST:
            for (size_t i = 0; i < node->data.decl_list.num_decls; i++) {
                ast_dump_node(node->data.decl_list.decls[i], out, indent + 1);
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
            print_indent(out, indent + 1);
            fprintf(out, "Cond:\n");
            ast_dump_node(node->data.if_stmt.cond, out, indent + 2);
            print_indent(out, indent + 1);
            fprintf(out, "Then:\n");
            ast_dump_node(node->data.if_stmt.then_stmt, out, indent + 2);
            if (node->data.if_stmt.else_stmt) {
                print_indent(out, indent + 1);
                fprintf(out, "Else:\n");
                ast_dump_node(node->data.if_stmt.else_stmt, out, indent + 2);
            }
            break;
            
        case AST_WHILE_STMT:
            print_indent(out, indent + 1);
            fprintf(out, "Cond:\n");
            ast_dump_node(node->data.while_stmt.cond, out, indent + 2);
            print_indent(out, indent + 1);
            fprintf(out, "Body:\n");
            ast_dump_node(node->data.while_stmt.body, out, indent + 2);
            break;
            
        case AST_DO_STMT:
            print_indent(out, indent + 1);
            fprintf(out, "Body:\n");
            ast_dump_node(node->data.do_stmt.body, out, indent + 2);
            print_indent(out, indent + 1);
            fprintf(out, "Cond:\n");
            ast_dump_node(node->data.do_stmt.cond, out, indent + 2);
            break;
            
        case AST_FOR_STMT:
            if (node->data.for_stmt.init_decl) {
                print_indent(out, indent + 1);
                fprintf(out, "InitDecl:\n");
                ast_dump_node(node->data.for_stmt.init_decl, out, indent + 2);
            }
            if (node->data.for_stmt.init) {
                print_indent(out, indent + 1);
                fprintf(out, "Init:\n");
                ast_dump_node(node->data.for_stmt.init, out, indent + 2);
            }
            if (node->data.for_stmt.cond) {
                print_indent(out, indent + 1);
                fprintf(out, "Cond:\n");
                ast_dump_node(node->data.for_stmt.cond, out, indent + 2);
            }
            if (node->data.for_stmt.incr) {
                print_indent(out, indent + 1);
                fprintf(out, "Incr:\n");
                ast_dump_node(node->data.for_stmt.incr, out, indent + 2);
            }
            print_indent(out, indent + 1);
            fprintf(out, "Body:\n");
            ast_dump_node(node->data.for_stmt.body, out, indent + 2);
            break;
            
        case AST_SWITCH_STMT:
            print_indent(out, indent + 1);
            fprintf(out, "Expr:\n");
            ast_dump_node(node->data.switch_stmt.expr, out, indent + 2);
            print_indent(out, indent + 1);
            fprintf(out, "Body:\n");
            ast_dump_node(node->data.switch_stmt.body, out, indent + 2);
            break;
            
        case AST_CASE_STMT:
            print_indent(out, indent + 1);
            fprintf(out, "Value:\n");
            ast_dump_node(node->data.case_stmt.expr, out, indent + 2);
            if (node->data.case_stmt.end_expr) {
                print_indent(out, indent + 1);
                fprintf(out, "EndValue (range):\n");
                ast_dump_node(node->data.case_stmt.end_expr, out, indent + 2);
            }
            if (node->data.case_stmt.stmt) {
                ast_dump_node(node->data.case_stmt.stmt, out, indent + 1);
            }
            break;
            
        case AST_DEFAULT_STMT:
            ast_dump_node(node->data.default_stmt.stmt, out, indent + 1);
            break;
            
        case AST_RETURN_STMT:
            if (node->data.return_stmt.expr) {
                print_indent(out, indent + 1);
                fprintf(out, "Value:\n");
                ast_dump_node(node->data.return_stmt.expr, out, indent + 2);
            }
            break;
            
        case AST_LABEL_STMT:
            ast_dump_node(node->data.label_stmt.stmt, out, indent + 1);
            break;
            
        case AST_BINARY_EXPR:
            print_indent(out, indent + 1);
            fprintf(out, "LHS:\n");
            ast_dump_node(node->data.binary_expr.lhs, out, indent + 2);
            print_indent(out, indent + 1);
            fprintf(out, "RHS:\n");
            ast_dump_node(node->data.binary_expr.rhs, out, indent + 2);
            break;
            
        case AST_UNARY_EXPR:
            ast_dump_node(node->data.unary_expr.operand, out, indent + 1);
            break;
            
        case AST_TERNARY_EXPR:
            print_indent(out, indent + 1);
            fprintf(out, "Cond:\n");
            ast_dump_node(node->data.ternary_expr.cond, out, indent + 2);
            print_indent(out, indent + 1);
            fprintf(out, "Then:\n");
            ast_dump_node(node->data.ternary_expr.then_expr, out, indent + 2);
            print_indent(out, indent + 1);
            fprintf(out, "Else:\n");
            ast_dump_node(node->data.ternary_expr.else_expr, out, indent + 2);
            break;
            
        case AST_CALL_EXPR:
            print_indent(out, indent + 1);
            fprintf(out, "Callee:\n");
            ast_dump_node(node->data.call_expr.func, out, indent + 2);
            if (node->data.call_expr.num_args > 0) {
                print_indent(out, indent + 1);
                fprintf(out, "Args:\n");
                for (size_t i = 0; i < node->data.call_expr.num_args; i++) {
                    ast_dump_node(node->data.call_expr.args[i], out, indent + 2);
                }
            }
            break;
            
        case AST_SUBSCRIPT_EXPR:
            print_indent(out, indent + 1);
            fprintf(out, "Array:\n");
            ast_dump_node(node->data.subscript_expr.array, out, indent + 2);
            print_indent(out, indent + 1);
            fprintf(out, "Index:\n");
            ast_dump_node(node->data.subscript_expr.index, out, indent + 2);
            break;
            
        case AST_MEMBER_EXPR:
            print_indent(out, indent + 1);
            fprintf(out, "Object:\n");
            ast_dump_node(node->data.member_expr.object, out, indent + 2);
            break;
            
        case AST_CAST_EXPR:
            print_indent(out, indent + 1);
            fprintf(out, "Expr:\n");
            ast_dump_node(node->data.cast_expr.expr, out, indent + 2);
            break;
            
        case AST_SIZEOF_EXPR:
            if (node->data.sizeof_expr.expr_arg) {
                ast_dump_node(node->data.sizeof_expr.expr_arg, out, indent + 1);
            }
            break;
            
        case AST_COMMA_EXPR:
            print_indent(out, indent + 1);
            fprintf(out, "Left:\n");
            ast_dump_node(node->data.comma_expr.left, out, indent + 2);
            print_indent(out, indent + 1);
            fprintf(out, "Right:\n");
            ast_dump_node(node->data.comma_expr.right, out, indent + 2);
            break;
            
        case AST_STRUCT_DECL:
        case AST_UNION_DECL:
            if (node->data.struct_decl.num_fields > 0) {
                print_indent(out, indent + 1);
                fprintf(out, "Fields:\n");
                for (size_t i = 0; i < node->data.struct_decl.num_fields; i++) {
                    ast_dump_node(node->data.struct_decl.fields[i], out, indent + 2);
                }
            }
            break;
            
        case AST_ENUM_DECL:
            if (node->data.enum_decl.enumerators && node->data.enum_decl.num_enumerators > 0) {
                print_indent(out, indent + 1);
                fprintf(out, "Enumerators:\n");
                for (size_t i = 0; i < node->data.enum_decl.num_enumerators; i++) {
                    ast_dump_node(node->data.enum_decl.enumerators[i], out, indent + 2);
                }
            } else if (node->data.enum_decl.enum_type && node->data.enum_decl.num_enumerators > 0) {
                /* Enum constants stored in type, dump from type */
                print_indent(out, indent + 1);
                fprintf(out, "Constants:\n");
                mcc_type_t *et = node->data.enum_decl.enum_type;
                for (mcc_enum_const_t *c = et->data.enumeration.constants; c; c = c->next) {
                    print_indent(out, indent + 2);
                    fprintf(out, "'%s' = %lld\n", c->name, (long long)c->value);
                }
            }
            break;
            
        case AST_ENUMERATOR:
            if (node->data.enumerator.value) {
                print_indent(out, indent + 1);
                fprintf(out, "ExplicitValue:\n");
                ast_dump_node(node->data.enumerator.value, out, indent + 2);
            }
            break;
            
        case AST_FIELD_DECL:
            if (node->data.field_decl.bitfield) {
                print_indent(out, indent + 1);
                fprintf(out, "BitWidth:\n");
                ast_dump_node(node->data.field_decl.bitfield, out, indent + 2);
            }
            break;
            
        case AST_COMPOUND_LIT:
            if (node->data.compound_lit.init) {
                print_indent(out, indent + 1);
                fprintf(out, "Init:\n");
                ast_dump_node(node->data.compound_lit.init, out, indent + 2);
            }
            break;
            
        case AST_DESIGNATED_INIT:
            if (node->data.designated_init.designator) {
                print_indent(out, indent + 1);
                fprintf(out, "Designator:\n");
                ast_dump_node(node->data.designated_init.designator, out, indent + 2);
            }
            if (node->data.designated_init.value) {
                print_indent(out, indent + 1);
                fprintf(out, "Value:\n");
                ast_dump_node(node->data.designated_init.value, out, indent + 2);
            }
            break;
            
        case AST_STATIC_ASSERT:
            if (node->data.static_assert.expr) {
                print_indent(out, indent + 1);
                fprintf(out, "Expr:\n");
                ast_dump_node(node->data.static_assert.expr, out, indent + 2);
            }
            break;
            
        case AST_GENERIC_EXPR:
            if (node->data.generic_expr.controlling_expr) {
                print_indent(out, indent + 1);
                fprintf(out, "ControllingExpr:\n");
                ast_dump_node(node->data.generic_expr.controlling_expr, out, indent + 2);
            }
            /* TODO: dump associations */
            if (node->data.generic_expr.default_expr) {
                print_indent(out, indent + 1);
                fprintf(out, "Default:\n");
                ast_dump_node(node->data.generic_expr.default_expr, out, indent + 2);
            }
            break;
            
        case AST_STMT_EXPR:
            if (node->data.stmt_expr.stmt) {
                ast_dump_node(node->data.stmt_expr.stmt, out, indent + 1);
            }
            break;
            
        case AST_GOTO_EXPR:
            if (node->data.goto_expr.expr) {
                print_indent(out, indent + 1);
                fprintf(out, "Target:\n");
                ast_dump_node(node->data.goto_expr.expr, out, indent + 2);
            }
            break;
            
        case AST_ALIGNOF_EXPR:
            if (node->data.alignof_expr.expr_arg) {
                print_indent(out, indent + 1);
                fprintf(out, "Expr:\n");
                ast_dump_node(node->data.alignof_expr.expr_arg, out, indent + 2);
            }
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

/*
 * Print AST to stdout (legacy function)
 */
void mcc_ast_print(mcc_ast_node_t *node, int indent)
{
    (void)indent;  /* unused */
    mcc_ast_dump(node, stdout);
}

/*
 * Dump AST to file
 */
void mcc_ast_dump(mcc_ast_node_t *node, FILE *out)
{
    ast_dump_node(node, out, 0);
}
