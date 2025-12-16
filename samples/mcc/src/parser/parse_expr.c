/*
 * MCC - Micro C Compiler
 * Expression Parser
 * 
 * This file handles parsing of all C expressions:
 * - Primary expressions (literals, identifiers, parenthesized)
 * - Postfix expressions ([], (), ., ->, ++, --)
 * - Unary expressions (++, --, &, *, +, -, ~, !, sizeof)
 * - Binary expressions (arithmetic, comparison, logical, bitwise)
 * - Ternary expressions (?:)
 * - Assignment expressions
 * - Comma expressions
 */

#include "parse_internal.h"

/* ============================================================
 * Primary Expression
 * ============================================================ */

mcc_ast_node_t *parse_primary(mcc_parser_t *p)
{
    mcc_token_t *tok = p->peek;
    mcc_ast_node_t *node;
    
    switch (tok->type) {
        case TOK_INT_LIT:
            parse_advance(p);
            node = mcc_ast_create(p->ctx, AST_INT_LIT, tok->location);
            node->data.int_lit.value = tok->literal.int_val.value;
            node->data.int_lit.suffix = tok->literal.int_val.suffix;
            return node;
            
        case TOK_FLOAT_LIT:
            parse_advance(p);
            node = mcc_ast_create(p->ctx, AST_FLOAT_LIT, tok->location);
            node->data.float_lit.value = tok->literal.float_val.value;
            node->data.float_lit.suffix = tok->literal.float_val.suffix;
            return node;
            
        case TOK_CHAR_LIT:
            parse_advance(p);
            node = mcc_ast_create(p->ctx, AST_CHAR_LIT, tok->location);
            node->data.char_lit.value = tok->literal.char_val.value;
            return node;
            
        case TOK_STRING_LIT:
            parse_advance(p);
            node = mcc_ast_create(p->ctx, AST_STRING_LIT, tok->location);
            node->data.string_lit.value = tok->literal.string_val.value;
            node->data.string_lit.length = tok->literal.string_val.length;
            return node;
            
        case TOK_IDENT:
            parse_advance(p);
            node = mcc_ast_create(p->ctx, AST_IDENT_EXPR, tok->location);
            node->data.ident_expr.name = mcc_strdup(p->ctx, tok->text);
            return node;
        
        /* C23: true/false keywords */
        case TOK_TRUE:
            if (!parse_has_true_false(p)) {
                mcc_warning_at(p->ctx, tok->location, 
                    "'true' as keyword is a C23 extension");
            }
            parse_advance(p);
            node = mcc_ast_create(p->ctx, AST_INT_LIT, tok->location);
            node->data.int_lit.value = 1;
            node->data.int_lit.suffix = 0;
            return node;
            
        case TOK_FALSE:
            if (!parse_has_true_false(p)) {
                mcc_warning_at(p->ctx, tok->location,
                    "'false' as keyword is a C23 extension");
            }
            parse_advance(p);
            node = mcc_ast_create(p->ctx, AST_INT_LIT, tok->location);
            node->data.int_lit.value = 0;
            node->data.int_lit.suffix = 0;
            return node;
        
        /* C23: nullptr */
        case TOK_NULLPTR:
            if (!parse_has_nullptr(p)) {
                mcc_warning_at(p->ctx, tok->location,
                    "'nullptr' is a C23 extension");
            }
            parse_advance(p);
            node = mcc_ast_create(p->ctx, AST_NULL_PTR, tok->location);
            return node;
            
        case TOK_LPAREN: {
            parse_advance(p);
            
            /* Check for cast expression or compound literal */
            if (parse_is_type_start(p)) {
                mcc_type_t *base_type = parse_type_specifier(p);
                mcc_type_t *type = parse_abstract_declarator(p, base_type);
                parse_expect(p, TOK_RPAREN, ")");
                
                /* Check for compound literal (C99) */
                if (parse_check(p, TOK_LBRACE)) {
                    if (!parse_has_compound_lit(p)) {
                        mcc_warning_at(p->ctx, tok->location,
                            "compound literals are a C99 extension");
                    }
                    mcc_ast_node_t *init = parse_initializer(p);
                    node = mcc_ast_create(p->ctx, AST_COMPOUND_LIT, tok->location);
                    node->data.compound_lit.type = type;
                    node->data.compound_lit.init = init;
                    return node;
                }
                
                /* Cast expression */
                mcc_ast_node_t *expr = parse_unary(p);
                node = mcc_ast_create(p->ctx, AST_CAST_EXPR, tok->location);
                node->data.cast_expr.target_type = type;
                node->data.cast_expr.expr = expr;
                return node;
            }
            
            /* GNU: Statement expression ({ ... }) */
            if (parse_check(p, TOK_LBRACE) && parse_has_stmt_expr(p)) {
                mcc_ast_node_t *stmt = parse_compound_stmt(p);
                parse_expect(p, TOK_RPAREN, ")");
                node = mcc_ast_create(p->ctx, AST_STMT_EXPR, tok->location);
                node->data.stmt_expr.stmt = stmt;
                return node;
            }
            
            /* Parenthesized expression */
            node = parse_expression(p);
            parse_expect(p, TOK_RPAREN, ")");
            return node;
        }
            
        case TOK_SIZEOF:
            parse_advance(p);
            node = mcc_ast_create(p->ctx, AST_SIZEOF_EXPR, tok->location);
            
            if (parse_match(p, TOK_LPAREN)) {
                if (parse_is_type_start(p)) {
                    mcc_type_t *base_type = parse_type_specifier(p);
                    node->data.sizeof_expr.type_arg = parse_abstract_declarator(p, base_type);
                } else {
                    node->data.sizeof_expr.expr_arg = parse_expression(p);
                }
                parse_expect(p, TOK_RPAREN, ")");
            } else {
                node->data.sizeof_expr.expr_arg = parse_unary(p);
            }
            return node;
        
        /* C11: _Alignof */
        case TOK__ALIGNOF:
            if (!parse_has_alignof(p)) {
                mcc_warning_at(p->ctx, tok->location,
                    "'_Alignof' is a C11 extension");
            }
            parse_advance(p);
            parse_expect(p, TOK_LPAREN, "(");
            node = mcc_ast_create(p->ctx, AST_ALIGNOF_EXPR, tok->location);
            node->data.alignof_expr.type_arg = parse_type_specifier(p);
            parse_expect(p, TOK_RPAREN, ")");
            return node;
        
        /* C23: alignof (without underscore) */
        case TOK_ALIGNOF:
            if (!parse_has_alignof(p)) {
                mcc_warning_at(p->ctx, tok->location,
                    "'alignof' is a C11/C23 extension");
            }
            parse_advance(p);
            parse_expect(p, TOK_LPAREN, "(");
            node = mcc_ast_create(p->ctx, AST_ALIGNOF_EXPR, tok->location);
            node->data.alignof_expr.type_arg = parse_type_specifier(p);
            parse_expect(p, TOK_RPAREN, ")");
            return node;
        
        /* C11: _Generic selection */
        case TOK__GENERIC: {
            if (!parse_has_generic(p)) {
                mcc_error_at(p->ctx, tok->location,
                    "'_Generic' requires C11 or later");
                return NULL;
            }
            parse_advance(p);
            parse_expect(p, TOK_LPAREN, "(");
            
            /* Parse controlling expression */
            mcc_ast_node_t *ctrl_expr = parse_assignment_expr(p);
            parse_expect(p, TOK_COMMA, ",");
            
            /* Parse association list */
            mcc_generic_assoc_t *assocs = NULL;
            mcc_generic_assoc_t *assoc_tail = NULL;
            int num_assocs = 0;
            mcc_ast_node_t *default_assoc_expr = NULL;
            
            while (!parse_check(p, TOK_RPAREN) && !parse_check(p, TOK_EOF)) {
                if (parse_match(p, TOK_DEFAULT)) {
                    parse_expect(p, TOK_COLON, ":");
                    if (default_assoc_expr) {
                        mcc_error_at(p->ctx, p->peek->location,
                            "duplicate default association in _Generic");
                    }
                    default_assoc_expr = parse_assignment_expr(p);
                } else {
                    /* type-name : assignment-expression */
                    mcc_type_t *assoc_type = parse_type_specifier(p);
                    assoc_type = parse_abstract_declarator(p, assoc_type);
                    parse_expect(p, TOK_COLON, ":");
                    mcc_ast_node_t *assoc_expr = parse_assignment_expr(p);
                    
                    /* Create association entry */
                    mcc_generic_assoc_t *assoc = mcc_alloc(p->ctx, sizeof(mcc_generic_assoc_t));
                    assoc->type = assoc_type;
                    assoc->expr = assoc_expr;
                    assoc->next = NULL;
                    
                    if (!assocs) assocs = assoc;
                    if (assoc_tail) assoc_tail->next = assoc;
                    assoc_tail = assoc;
                    num_assocs++;
                }
                
                if (!parse_match(p, TOK_COMMA)) {
                    break;
                }
            }
            
            parse_expect(p, TOK_RPAREN, ")");
            
            /* Create _Generic AST node with all associations */
            node = mcc_ast_create(p->ctx, AST_GENERIC_EXPR, tok->location);
            node->data.generic_expr.controlling_expr = ctrl_expr;
            node->data.generic_expr.associations = assocs;
            node->data.generic_expr.num_associations = num_assocs;
            node->data.generic_expr.default_expr = default_assoc_expr;
            return node;
        }
        
        /* GNU: Labels as values */
        case TOK_AND:
            if (parse_has_label_addr(p)) {
                parse_advance(p);
                if (parse_check(p, TOK_IDENT)) {
                    mcc_token_t *label = p->peek;
                    parse_advance(p);
                    node = mcc_ast_create(p->ctx, AST_LABEL_ADDR, tok->location);
                    node->data.label_addr.label = mcc_strdup(p->ctx, label->text);
                    return node;
                }
            }
            /* Fall through to error */
            /* FALLTHROUGH */
            
        default:
            mcc_error_at(p->ctx, tok->location, "expected expression, got '%s'",
                         mcc_token_to_string(tok));
            return NULL;
    }
}

/* ============================================================
 * Postfix Expression
 * ============================================================ */

mcc_ast_node_t *parse_postfix(mcc_parser_t *p)
{
    mcc_ast_node_t *node = parse_primary(p);
    if (!node) return NULL;
    
    while (1) {
        mcc_token_t *tok = p->peek;
        
        if (parse_match(p, TOK_LBRACKET)) {
            /* Array subscript */
            mcc_ast_node_t *index = parse_expression(p);
            parse_expect(p, TOK_RBRACKET, "]");
            
            mcc_ast_node_t *subscript = mcc_ast_create(p->ctx, AST_SUBSCRIPT_EXPR, tok->location);
            subscript->data.subscript_expr.array = node;
            subscript->data.subscript_expr.index = index;
            node = subscript;
            
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
            
            mcc_ast_node_t *call = mcc_ast_create(p->ctx, AST_CALL_EXPR, tok->location);
            call->data.call_expr.func = node;
            call->data.call_expr.args = args;
            call->data.call_expr.num_args = num_args;
            node = call;
            
        } else if (parse_match(p, TOK_DOT)) {
            /* Member access */
            mcc_token_t *member = parse_expect(p, TOK_IDENT, "member name");
            
            mcc_ast_node_t *mem = mcc_ast_create(p->ctx, AST_MEMBER_EXPR, tok->location);
            mem->data.member_expr.object = node;
            mem->data.member_expr.member = mcc_strdup(p->ctx, member->text);
            mem->data.member_expr.is_arrow = false;
            node = mem;
            
        } else if (parse_match(p, TOK_ARROW)) {
            /* Pointer member access */
            mcc_token_t *member = parse_expect(p, TOK_IDENT, "member name");
            
            mcc_ast_node_t *mem = mcc_ast_create(p->ctx, AST_MEMBER_EXPR, tok->location);
            mem->data.member_expr.object = node;
            mem->data.member_expr.member = mcc_strdup(p->ctx, member->text);
            mem->data.member_expr.is_arrow = true;
            node = mem;
            
        } else if (parse_match(p, TOK_INC)) {
            /* Post-increment */
            mcc_ast_node_t *inc = mcc_ast_create(p->ctx, AST_UNARY_EXPR, tok->location);
            inc->data.unary_expr.op = UNOP_POST_INC;
            inc->data.unary_expr.operand = node;
            node = inc;
            
        } else if (parse_match(p, TOK_DEC)) {
            /* Post-decrement */
            mcc_ast_node_t *dec = mcc_ast_create(p->ctx, AST_UNARY_EXPR, tok->location);
            dec->data.unary_expr.op = UNOP_POST_DEC;
            dec->data.unary_expr.operand = node;
            node = dec;
            
        } else {
            break;
        }
    }
    
    return node;
}

/* ============================================================
 * Unary Expression
 * ============================================================ */

mcc_ast_node_t *parse_unary(mcc_parser_t *p)
{
    mcc_token_t *tok = p->peek;
    mcc_unop_t op;
    
    switch (tok->type) {
        case TOK_INC:
            parse_advance(p);
            op = UNOP_PRE_INC;
            break;
        case TOK_DEC:
            parse_advance(p);
            op = UNOP_PRE_DEC;
            break;
        case TOK_AMP:
            parse_advance(p);
            op = UNOP_ADDR;
            break;
        case TOK_STAR:
            parse_advance(p);
            op = UNOP_DEREF;
            break;
        case TOK_PLUS:
            parse_advance(p);
            op = UNOP_POS;
            break;
        case TOK_MINUS:
            parse_advance(p);
            op = UNOP_NEG;
            break;
        case TOK_TILDE:
            parse_advance(p);
            op = UNOP_BIT_NOT;
            break;
        case TOK_NOT:
            parse_advance(p);
            op = UNOP_NOT;
            break;
        default:
            return parse_postfix(p);
    }
    
    mcc_ast_node_t *operand = parse_unary(p);
    mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_UNARY_EXPR, tok->location);
    node->data.unary_expr.op = op;
    node->data.unary_expr.operand = operand;
    return node;
}

/* ============================================================
 * Binary Expression (Precedence Climbing)
 * ============================================================ */

mcc_ast_node_t *parse_binary(mcc_parser_t *p, int min_prec)
{
    mcc_ast_node_t *left = parse_unary(p);
    if (!left) return NULL;
    
    while (1) {
        mcc_token_t *tok = p->peek;
        int prec;
        mcc_binop_t op;
        bool right_assoc = false;
        
        switch (tok->type) {
            case TOK_STAR:      prec = 13; op = BINOP_MUL; break;
            case TOK_SLASH:     prec = 13; op = BINOP_DIV; break;
            case TOK_PERCENT:   prec = 13; op = BINOP_MOD; break;
            case TOK_PLUS:      prec = 12; op = BINOP_ADD; break;
            case TOK_MINUS:     prec = 12; op = BINOP_SUB; break;
            case TOK_LSHIFT:    prec = 11; op = BINOP_LSHIFT; break;
            case TOK_RSHIFT:    prec = 11; op = BINOP_RSHIFT; break;
            case TOK_LT:        prec = 10; op = BINOP_LT; break;
            case TOK_GT:        prec = 10; op = BINOP_GT; break;
            case TOK_LE:        prec = 10; op = BINOP_LE; break;
            case TOK_GE:        prec = 10; op = BINOP_GE; break;
            case TOK_EQ:        prec = 9;  op = BINOP_EQ; break;
            case TOK_NE:        prec = 9;  op = BINOP_NE; break;
            case TOK_AMP:       prec = 8;  op = BINOP_BIT_AND; break;
            case TOK_CARET:     prec = 7;  op = BINOP_BIT_XOR; break;
            case TOK_PIPE:      prec = 6;  op = BINOP_BIT_OR; break;
            case TOK_AND:       prec = 5;  op = BINOP_AND; break;
            case TOK_OR:        prec = 4;  op = BINOP_OR; break;
            default:
                return left;
        }
        
        if (prec < min_prec) {
            return left;
        }
        
        parse_advance(p);
        
        int next_prec = right_assoc ? prec : prec + 1;
        mcc_ast_node_t *right = parse_binary(p, next_prec);
        
        mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_BINARY_EXPR, tok->location);
        node->data.binary_expr.op = op;
        node->data.binary_expr.lhs = left;
        node->data.binary_expr.rhs = right;
        left = node;
    }
}

/* ============================================================
 * Ternary Expression
 * ============================================================ */

mcc_ast_node_t *parse_ternary(mcc_parser_t *p)
{
    mcc_ast_node_t *cond = parse_binary(p, 1);
    if (!cond) return NULL;
    
    if (parse_match(p, TOK_QUESTION)) {
        mcc_location_t loc = p->current->location;
        mcc_ast_node_t *then_expr = parse_expression(p);
        parse_expect(p, TOK_COLON, ":");
        mcc_ast_node_t *else_expr = parse_ternary(p);
        
        mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_TERNARY_EXPR, loc);
        node->data.ternary_expr.cond = cond;
        node->data.ternary_expr.then_expr = then_expr;
        node->data.ternary_expr.else_expr = else_expr;
        return node;
    }
    
    return cond;
}

/* ============================================================
 * Assignment Expression
 * ============================================================ */

mcc_ast_node_t *parse_assignment_expr(mcc_parser_t *p)
{
    mcc_ast_node_t *left = parse_ternary(p);
    if (!left) return NULL;
    
    mcc_token_t *tok = p->peek;
    mcc_binop_t op;
    
    switch (tok->type) {
        case TOK_ASSIGN:        op = BINOP_ASSIGN; break;
        case TOK_PLUS_ASSIGN:   op = BINOP_ADD_ASSIGN; break;
        case TOK_MINUS_ASSIGN:  op = BINOP_SUB_ASSIGN; break;
        case TOK_STAR_ASSIGN:   op = BINOP_MUL_ASSIGN; break;
        case TOK_SLASH_ASSIGN:  op = BINOP_DIV_ASSIGN; break;
        case TOK_PERCENT_ASSIGN:op = BINOP_MOD_ASSIGN; break;
        case TOK_AMP_ASSIGN:    op = BINOP_AND_ASSIGN; break;
        case TOK_PIPE_ASSIGN:   op = BINOP_OR_ASSIGN; break;
        case TOK_CARET_ASSIGN:  op = BINOP_XOR_ASSIGN; break;
        case TOK_LSHIFT_ASSIGN: op = BINOP_LSHIFT_ASSIGN; break;
        case TOK_RSHIFT_ASSIGN: op = BINOP_RSHIFT_ASSIGN; break;
        default:
            return left;
    }
    
    parse_advance(p);
    mcc_ast_node_t *right = parse_assignment_expr(p);
    
    mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_BINARY_EXPR, tok->location);
    node->data.binary_expr.op = op;
    node->data.binary_expr.lhs = left;
    node->data.binary_expr.rhs = right;
    return node;
}

/* ============================================================
 * Comma Expression
 * ============================================================ */

mcc_ast_node_t *parse_expression(mcc_parser_t *p)
{
    mcc_ast_node_t *left = parse_assignment_expr(p);
    if (!left) return NULL;
    
    while (parse_match(p, TOK_COMMA)) {
        mcc_location_t loc = p->current->location;
        mcc_ast_node_t *right = parse_assignment_expr(p);
        
        mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_COMMA_EXPR, loc);
        node->data.comma_expr.left = left;
        node->data.comma_expr.right = right;
        left = node;
    }
    
    return left;
}

/* ============================================================
 * Constant Expression
 * ============================================================ */

mcc_ast_node_t *parse_constant_expr(mcc_parser_t *p)
{
    /* Constant expression is a conditional expression that can be
     * evaluated at compile time. For now, just parse as ternary. */
    return parse_ternary(p);
}
