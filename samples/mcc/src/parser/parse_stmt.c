/*
 * MCC - Micro C Compiler
 * Statement Parser
 * 
 * This file handles parsing of all C statements:
 * - Compound statements { ... }
 * - Selection statements (if, switch)
 * - Iteration statements (while, do, for)
 * - Jump statements (goto, continue, break, return)
 * - Labeled statements (label:, case, default)
 * - Expression statements
 */

#include "parse_internal.h"

/* ============================================================
 * Compound Statement
 * ============================================================ */

mcc_ast_node_t *parse_compound_stmt(mcc_parser_t *p)
{
    mcc_location_t loc = p->peek->location;
    parse_expect(p, TOK_LBRACE, "{");
    
    mcc_ast_node_t **stmts = NULL;
    size_t num_stmts = 0;
    size_t cap_stmts = 0;
    
    while (!parse_check(p, TOK_RBRACE) && !parse_check(p, TOK_EOF)) {
        mcc_ast_node_t *stmt = NULL;
        
        if (parse_is_declaration_start(p)) {
            stmt = parse_declaration(p);
        } else {
            stmt = parse_statement(p);
        }
        
        if (stmt) {
            if (num_stmts >= cap_stmts) {
                cap_stmts = cap_stmts ? cap_stmts * 2 : 8;
                stmts = mcc_realloc(p->ctx, stmts,
                                    num_stmts * sizeof(mcc_ast_node_t*),
                                    cap_stmts * sizeof(mcc_ast_node_t*));
            }
            stmts[num_stmts++] = stmt;
        }
        
        if (p->panic_mode) {
            parse_synchronize(p);
        }
    }
    
    parse_expect(p, TOK_RBRACE, "}");
    
    mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_COMPOUND_STMT, loc);
    node->data.compound_stmt.stmts = stmts;
    node->data.compound_stmt.num_stmts = num_stmts;
    return node;
}

/* ============================================================
 * If Statement
 * ============================================================ */

mcc_ast_node_t *parse_if_stmt(mcc_parser_t *p)
{
    mcc_token_t *tok = p->peek;
    parse_advance(p); /* consume 'if' */
    
    parse_expect(p, TOK_LPAREN, "(");
    mcc_ast_node_t *cond = parse_expression(p);
    parse_expect(p, TOK_RPAREN, ")");
    
    mcc_ast_node_t *then_stmt = parse_statement(p);
    mcc_ast_node_t *else_stmt = NULL;
    
    if (parse_match(p, TOK_ELSE)) {
        else_stmt = parse_statement(p);
    }
    
    mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_IF_STMT, tok->location);
    node->data.if_stmt.cond = cond;
    node->data.if_stmt.then_stmt = then_stmt;
    node->data.if_stmt.else_stmt = else_stmt;
    return node;
}

/* ============================================================
 * While Statement
 * ============================================================ */

mcc_ast_node_t *parse_while_stmt(mcc_parser_t *p)
{
    mcc_token_t *tok = p->peek;
    parse_advance(p); /* consume 'while' */
    
    parse_expect(p, TOK_LPAREN, "(");
    mcc_ast_node_t *cond = parse_expression(p);
    parse_expect(p, TOK_RPAREN, ")");
    
    mcc_ast_node_t *body = parse_statement(p);
    
    mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_WHILE_STMT, tok->location);
    node->data.while_stmt.cond = cond;
    node->data.while_stmt.body = body;
    return node;
}

/* ============================================================
 * Do-While Statement
 * ============================================================ */

mcc_ast_node_t *parse_do_stmt(mcc_parser_t *p)
{
    mcc_token_t *tok = p->peek;
    parse_advance(p); /* consume 'do' */
    
    mcc_ast_node_t *body = parse_statement(p);
    
    parse_expect(p, TOK_WHILE, "while");
    parse_expect(p, TOK_LPAREN, "(");
    mcc_ast_node_t *cond = parse_expression(p);
    parse_expect(p, TOK_RPAREN, ")");
    parse_expect(p, TOK_SEMICOLON, ";");
    
    mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_DO_STMT, tok->location);
    node->data.do_stmt.body = body;
    node->data.do_stmt.cond = cond;
    return node;
}

/* ============================================================
 * For Statement
 * ============================================================ */

mcc_ast_node_t *parse_for_stmt(mcc_parser_t *p)
{
    mcc_token_t *tok = p->peek;
    parse_advance(p); /* consume 'for' */
    
    parse_expect(p, TOK_LPAREN, "(");
    
    mcc_ast_node_t *init = NULL;
    mcc_ast_node_t *init_decl = NULL;
    
    if (!parse_check(p, TOK_SEMICOLON)) {
        /* C99: Check for declaration in for init */
        if (parse_is_declaration_start(p)) {
            if (!parse_has_for_decl(p)) {
                mcc_warning_at(p->ctx, p->peek->location,
                    "declaration in for loop initializer is a C99 extension");
            }
            /* Parse declaration (without trailing semicolon check - 
               parse_declaration handles it) */
            init_decl = parse_declaration(p);
            /* Declaration already consumed semicolon, so skip the expect below */
            goto parse_cond;
        } else {
            init = parse_expression(p);
        }
    }
    parse_expect(p, TOK_SEMICOLON, ";");
    
parse_cond:
    ;
    mcc_ast_node_t *cond = NULL;
    if (!parse_check(p, TOK_SEMICOLON)) {
        cond = parse_expression(p);
    }
    parse_expect(p, TOK_SEMICOLON, ";");
    
    mcc_ast_node_t *incr = NULL;
    if (!parse_check(p, TOK_RPAREN)) {
        incr = parse_expression(p);
    }
    parse_expect(p, TOK_RPAREN, ")");
    
    mcc_ast_node_t *body = parse_statement(p);
    
    mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_FOR_STMT, tok->location);
    node->data.for_stmt.init = init;
    node->data.for_stmt.init_decl = init_decl;
    node->data.for_stmt.cond = cond;
    node->data.for_stmt.incr = incr;
    node->data.for_stmt.body = body;
    return node;
}

/* ============================================================
 * Switch Statement
 * ============================================================ */

mcc_ast_node_t *parse_switch_stmt(mcc_parser_t *p)
{
    mcc_token_t *tok = p->peek;
    parse_advance(p); /* consume 'switch' */
    
    parse_expect(p, TOK_LPAREN, "(");
    mcc_ast_node_t *expr = parse_expression(p);
    parse_expect(p, TOK_RPAREN, ")");
    
    mcc_ast_node_t *body = parse_statement(p);
    
    mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_SWITCH_STMT, tok->location);
    node->data.switch_stmt.expr = expr;
    node->data.switch_stmt.body = body;
    return node;
}

/* ============================================================
 * Return Statement
 * ============================================================ */

mcc_ast_node_t *parse_return_stmt(mcc_parser_t *p)
{
    mcc_token_t *tok = p->peek;
    parse_advance(p); /* consume 'return' */
    
    mcc_ast_node_t *expr = NULL;
    if (!parse_check(p, TOK_SEMICOLON)) {
        expr = parse_expression(p);
    }
    parse_expect(p, TOK_SEMICOLON, ";");
    
    mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_RETURN_STMT, tok->location);
    node->data.return_stmt.expr = expr;
    return node;
}

/* ============================================================
 * Goto Statement
 * ============================================================ */

mcc_ast_node_t *parse_goto_stmt(mcc_parser_t *p)
{
    mcc_token_t *tok = p->peek;
    parse_advance(p); /* consume 'goto' */
    
    /* GNU: Computed goto (goto *expr) */
    if (parse_match(p, TOK_STAR)) {
        if (!parse_has_label_addr(p)) {
            mcc_warning_at(p->ctx, tok->location,
                "computed goto is a GNU extension");
        }
        mcc_ast_node_t *expr = parse_expression(p);
        parse_expect(p, TOK_SEMICOLON, ";");
        
        mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_GOTO_EXPR, tok->location);
        node->data.goto_expr.expr = expr;
        return node;
    }
    
    mcc_token_t *label = parse_expect(p, TOK_IDENT, "label");
    parse_expect(p, TOK_SEMICOLON, ";");
    
    mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_GOTO_STMT, tok->location);
    node->data.goto_stmt.label = mcc_strdup(p->ctx, label->text);
    return node;
}

/* ============================================================
 * Case Statement
 * ============================================================ */

static mcc_ast_node_t *parse_case_stmt(mcc_parser_t *p)
{
    mcc_token_t *tok = p->peek;
    parse_advance(p); /* consume 'case' */
    
    mcc_ast_node_t *expr = parse_constant_expr(p);
    
    /* GNU: Case ranges (case 1 ... 5:) */
    mcc_ast_node_t *end_expr = NULL;
    if (parse_match(p, TOK_ELLIPSIS)) {
        if (!parse_has_case_range(p)) {
            mcc_warning_at(p->ctx, tok->location,
                "case range is a GNU extension");
        }
        end_expr = parse_constant_expr(p);
    }
    
    parse_expect(p, TOK_COLON, ":");
    mcc_ast_node_t *stmt = parse_statement(p);
    
    mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_CASE_STMT, tok->location);
    node->data.case_stmt.expr = expr;
    node->data.case_stmt.end_expr = end_expr;  /* NULL if not a range */
    node->data.case_stmt.stmt = stmt;
    return node;
}

/* ============================================================
 * Default Statement
 * ============================================================ */

static mcc_ast_node_t *parse_default_stmt(mcc_parser_t *p)
{
    mcc_token_t *tok = p->peek;
    parse_advance(p); /* consume 'default' */
    
    parse_expect(p, TOK_COLON, ":");
    mcc_ast_node_t *stmt = parse_statement(p);
    
    mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_DEFAULT_STMT, tok->location);
    node->data.default_stmt.stmt = stmt;
    return node;
}

/* ============================================================
 * Labeled Statement
 * ============================================================ */

mcc_ast_node_t *parse_labeled_stmt(mcc_parser_t *p)
{
    mcc_token_t *tok = p->peek;
    
    /* We already know it's an identifier followed by ':' */
    parse_advance(p); /* consume identifier */
    parse_advance(p); /* consume ':' */
    
    mcc_ast_node_t *stmt = parse_statement(p);
    
    mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_LABEL_STMT, tok->location);
    node->data.label_stmt.label = mcc_strdup(p->ctx, tok->text);
    node->data.label_stmt.stmt = stmt;
    return node;
}

/* ============================================================
 * Statement (main dispatcher)
 * ============================================================ */

/* Skip C23 attributes [[...]] */
static void skip_attributes(mcc_parser_t *p)
{
    while (parse_check(p, TOK_LBRACKET2)) {
        if (!parse_has_feature(p, MCC_FEAT_ATTR_SYNTAX)) {
            mcc_warning_at(p->ctx, p->peek->location,
                "attribute syntax [[...]] is a C23 feature");
        }
        parse_advance(p);  /* Skip [[ */
        int depth = 1;
        while (depth > 0 && !parse_check(p, TOK_EOF)) {
            if (parse_check(p, TOK_LBRACKET2)) {
                depth++;
            } else if (parse_check(p, TOK_RBRACKET2)) {
                depth--;
            }
            parse_advance(p);
        }
    }
}

mcc_ast_node_t *parse_statement(mcc_parser_t *p)
{
    /* C23: Skip attributes [[...]] */
    skip_attributes(p);
    
    mcc_token_t *tok = p->peek;
    mcc_ast_node_t *node;
    
    switch (tok->type) {
        case TOK_LBRACE:
            return parse_compound_stmt(p);
            
        case TOK_IF:
            return parse_if_stmt(p);
            
        case TOK_WHILE:
            return parse_while_stmt(p);
            
        case TOK_DO:
            return parse_do_stmt(p);
            
        case TOK_FOR:
            return parse_for_stmt(p);
            
        case TOK_SWITCH:
            return parse_switch_stmt(p);
            
        case TOK_CASE:
            return parse_case_stmt(p);
            
        case TOK_DEFAULT:
            return parse_default_stmt(p);
            
        case TOK_BREAK:
            parse_advance(p);
            parse_expect(p, TOK_SEMICOLON, ";");
            return mcc_ast_create(p->ctx, AST_BREAK_STMT, tok->location);
            
        case TOK_CONTINUE:
            parse_advance(p);
            parse_expect(p, TOK_SEMICOLON, ";");
            return mcc_ast_create(p->ctx, AST_CONTINUE_STMT, tok->location);
            
        case TOK_RETURN:
            return parse_return_stmt(p);
            
        case TOK_GOTO:
            return parse_goto_stmt(p);
            
        case TOK_SEMICOLON:
            /* Null statement */
            parse_advance(p);
            return mcc_ast_create(p->ctx, AST_NULL_STMT, tok->location);
            
        case TOK_IDENT: {
            /* Check if it's a typedef name - then it's a declaration */
            if (parse_is_typedef_name(p, p->peek->text)) {
                /* C99: Mixed declarations and statements */
                if (!parse_has_mixed_decl(p)) {
                    mcc_warning_at(p->ctx, tok->location,
                        "mixing declarations and code is a C99 extension");
                }
                return parse_declaration(p);
            }
            
            /* Check for label - need to look ahead */
            mcc_token_t *ident_tok = p->peek;
            parse_advance(p);
            
            if (parse_check(p, TOK_COLON)) {
                /* It's a label */
                parse_advance(p);
                mcc_ast_node_t *stmt = parse_statement(p);
                
                node = mcc_ast_create(p->ctx, AST_LABEL_STMT, ident_tok->location);
                node->data.label_stmt.label = mcc_strdup(p->ctx, ident_tok->text);
                node->data.label_stmt.stmt = stmt;
                return node;
            }
            
            /* Not a label - build identifier expression and continue parsing */
            mcc_ast_node_t *ident = mcc_ast_create(p->ctx, AST_IDENT_EXPR, ident_tok->location);
            ident->data.ident_expr.name = mcc_strdup(p->ctx, ident_tok->text);
            
            /* Parse postfix operators (., ->, [], (), ++, --) */
            mcc_ast_node_t *expr = ident;
            while (1) {
                if (parse_match(p, TOK_DOT)) {
                    mcc_token_t *member = parse_expect(p, TOK_IDENT, "member name");
                    mcc_ast_node_t *mem = mcc_ast_create(p->ctx, AST_MEMBER_EXPR, ident_tok->location);
                    mem->data.member_expr.object = expr;
                    mem->data.member_expr.member = mcc_strdup(p->ctx, member->text);
                    mem->data.member_expr.is_arrow = false;
                    expr = mem;
                } else if (parse_match(p, TOK_ARROW)) {
                    mcc_token_t *member = parse_expect(p, TOK_IDENT, "member name");
                    mcc_ast_node_t *mem = mcc_ast_create(p->ctx, AST_MEMBER_EXPR, ident_tok->location);
                    mem->data.member_expr.object = expr;
                    mem->data.member_expr.member = mcc_strdup(p->ctx, member->text);
                    mem->data.member_expr.is_arrow = true;
                    expr = mem;
                } else if (parse_match(p, TOK_LBRACKET)) {
                    mcc_ast_node_t *index = parse_expression(p);
                    parse_expect(p, TOK_RBRACKET, "]");
                    mcc_ast_node_t *sub = mcc_ast_create(p->ctx, AST_SUBSCRIPT_EXPR, ident_tok->location);
                    sub->data.subscript_expr.array = expr;
                    sub->data.subscript_expr.index = index;
                    expr = sub;
                } else if (parse_match(p, TOK_LPAREN)) {
                    /* Function call */
                    mcc_ast_node_t **args = NULL;
                    size_t num_args = 0;
                    size_t cap_args = 0;
                    
                    if (!parse_check(p, TOK_RPAREN)) {
                        do {
                            mcc_ast_node_t *arg = parse_assignment_expr(p);
                            if (num_args >= cap_args) {
                                cap_args = cap_args ? cap_args * 2 : 4;
                                args = mcc_realloc(p->ctx, args,
                                                   num_args * sizeof(mcc_ast_node_t*),
                                                   cap_args * sizeof(mcc_ast_node_t*));
                            }
                            args[num_args++] = arg;
                        } while (parse_match(p, TOK_COMMA));
                    }
                    parse_expect(p, TOK_RPAREN, ")");
                    
                    mcc_ast_node_t *call = mcc_ast_create(p->ctx, AST_CALL_EXPR, ident_tok->location);
                    call->data.call_expr.func = expr;
                    call->data.call_expr.args = args;
                    call->data.call_expr.num_args = num_args;
                    expr = call;
                } else if (parse_match(p, TOK_INC)) {
                    mcc_ast_node_t *inc = mcc_ast_create(p->ctx, AST_UNARY_EXPR, ident_tok->location);
                    inc->data.unary_expr.op = UNOP_POST_INC;
                    inc->data.unary_expr.operand = expr;
                    expr = inc;
                } else if (parse_match(p, TOK_DEC)) {
                    mcc_ast_node_t *dec = mcc_ast_create(p->ctx, AST_UNARY_EXPR, ident_tok->location);
                    dec->data.unary_expr.op = UNOP_POST_DEC;
                    dec->data.unary_expr.operand = expr;
                    expr = dec;
                } else {
                    break;
                }
            }
            
            /* Handle assignment or other binary operators */
            if (mcc_token_is_assignment_op(p->peek->type)) {
                mcc_token_t *op_tok = p->peek;
                mcc_binop_t op;
                switch (op_tok->type) {
                    case TOK_ASSIGN: op = BINOP_ASSIGN; break;
                    case TOK_PLUS_ASSIGN: op = BINOP_ADD_ASSIGN; break;
                    case TOK_MINUS_ASSIGN: op = BINOP_SUB_ASSIGN; break;
                    case TOK_STAR_ASSIGN: op = BINOP_MUL_ASSIGN; break;
                    case TOK_SLASH_ASSIGN: op = BINOP_DIV_ASSIGN; break;
                    case TOK_PERCENT_ASSIGN: op = BINOP_MOD_ASSIGN; break;
                    case TOK_AMP_ASSIGN: op = BINOP_AND_ASSIGN; break;
                    case TOK_PIPE_ASSIGN: op = BINOP_OR_ASSIGN; break;
                    case TOK_CARET_ASSIGN: op = BINOP_XOR_ASSIGN; break;
                    case TOK_LSHIFT_ASSIGN: op = BINOP_LSHIFT_ASSIGN; break;
                    case TOK_RSHIFT_ASSIGN: op = BINOP_RSHIFT_ASSIGN; break;
                    default: op = BINOP_ASSIGN; break;
                }
                parse_advance(p);
                mcc_ast_node_t *rhs = parse_assignment_expr(p);
                mcc_ast_node_t *bin = mcc_ast_create(p->ctx, AST_BINARY_EXPR, op_tok->location);
                bin->data.binary_expr.op = op;
                bin->data.binary_expr.lhs = expr;
                bin->data.binary_expr.rhs = rhs;
                expr = bin;
            }
            
            parse_expect(p, TOK_SEMICOLON, ";");
            
            node = mcc_ast_create(p->ctx, AST_EXPR_STMT, ident_tok->location);
            node->data.expr_stmt.expr = expr;
            return node;
        }
            
        default: {
            /* Check for declaration at statement position (C99) */
            if (parse_is_declaration_start(p)) {
                if (!parse_has_mixed_decl(p)) {
                    mcc_warning_at(p->ctx, tok->location,
                        "mixing declarations and code is a C99 extension");
                }
                return parse_declaration(p);
            }
            
            /* Expression statement */
            mcc_ast_node_t *expr = parse_expression(p);
            parse_expect(p, TOK_SEMICOLON, ";");
            
            node = mcc_ast_create(p->ctx, AST_EXPR_STMT, tok->location);
            node->data.expr_stmt.expr = expr;
            return node;
        }
    }
}
