/*
 * MCC - Micro C Compiler
 * Parser implementation (recursive descent)
 */

#include "mcc.h"

/* Forward declarations */
static mcc_ast_node_t *parse_declaration(mcc_parser_t *p);
static mcc_ast_node_t *parse_statement(mcc_parser_t *p);
static mcc_ast_node_t *parse_expression(mcc_parser_t *p);
static mcc_ast_node_t *parse_assignment_expr(mcc_parser_t *p);
static mcc_type_t *parse_type_specifier(mcc_parser_t *p);

mcc_parser_t *mcc_parser_create(mcc_context_t *ctx, mcc_preprocessor_t *pp)
{
    mcc_parser_t *p = mcc_alloc(ctx, sizeof(mcc_parser_t));
    p->ctx = ctx;
    p->pp = pp;
    p->current = NULL;
    p->peek = NULL;
    return p;
}

void mcc_parser_destroy(mcc_parser_t *p)
{
    (void)p; /* Arena allocated */
}

mcc_token_t *mcc_parser_advance(mcc_parser_t *p)
{
    p->current = p->peek;
    p->peek = mcc_preprocessor_next(p->pp);
    
    /* Skip newlines */
    while (p->peek->type == TOK_NEWLINE) {
        p->peek = mcc_preprocessor_next(p->pp);
    }
    
    return p->current;
}

bool mcc_parser_check(mcc_parser_t *p, mcc_token_type_t type)
{
    return p->peek->type == type;
}

bool mcc_parser_match(mcc_parser_t *p, mcc_token_type_t type)
{
    if (mcc_parser_check(p, type)) {
        mcc_parser_advance(p);
        return true;
    }
    return false;
}

mcc_token_t *mcc_parser_expect(mcc_parser_t *p, mcc_token_type_t type, const char *msg)
{
    if (mcc_parser_check(p, type)) {
        return mcc_parser_advance(p);
    }
    
    mcc_error_at(p->ctx, p->peek->location, "Expected %s, got '%s'",
                 msg ? msg : mcc_token_type_name(type),
                 mcc_token_to_string(p->peek));
    p->panic_mode = true;
    return p->peek;
}

void mcc_parser_synchronize(mcc_parser_t *p)
{
    p->panic_mode = false;
    
    while (p->peek->type != TOK_EOF) {
        if (p->current && p->current->type == TOK_SEMICOLON) return;
        
        switch (p->peek->type) {
            case TOK_IF:
            case TOK_WHILE:
            case TOK_FOR:
            case TOK_DO:
            case TOK_SWITCH:
            case TOK_RETURN:
            case TOK_BREAK:
            case TOK_CONTINUE:
            case TOK_GOTO:
            case TOK_TYPEDEF:
            case TOK_EXTERN:
            case TOK_STATIC:
            case TOK_AUTO:
            case TOK_REGISTER:
            case TOK_VOID:
            case TOK_CHAR:
            case TOK_SHORT:
            case TOK_INT:
            case TOK_LONG:
            case TOK_FLOAT:
            case TOK_DOUBLE:
            case TOK_STRUCT:
            case TOK_UNION:
            case TOK_ENUM:
                return;
            default:
                break;
        }
        
        mcc_parser_advance(p);
    }
}

/* Check if current token starts a type */
static bool is_type_start(mcc_parser_t *p)
{
    switch (p->peek->type) {
        case TOK_VOID:
        case TOK_CHAR:
        case TOK_SHORT:
        case TOK_INT:
        case TOK_LONG:
        case TOK_FLOAT:
        case TOK_DOUBLE:
        case TOK_SIGNED:
        case TOK_UNSIGNED:
        case TOK_STRUCT:
        case TOK_UNION:
        case TOK_ENUM:
        case TOK_CONST:
        case TOK_VOLATILE:
            return true;
        case TOK_IDENT:
            /* Could be typedef name */
            if (p->symtab) {
                return mcc_symtab_is_typedef(p->symtab, p->peek->text);
            }
            return false;
        default:
            return false;
    }
}

/* Check if current token starts a declaration */
static bool is_declaration_start(mcc_parser_t *p)
{
    switch (p->peek->type) {
        case TOK_TYPEDEF:
        case TOK_EXTERN:
        case TOK_STATIC:
        case TOK_AUTO:
        case TOK_REGISTER:
            return true;
        default:
            return is_type_start(p);
    }
}

/* Parse primary expression */
static mcc_ast_node_t *parse_primary(mcc_parser_t *p)
{
    mcc_token_t *tok = p->peek;
    mcc_ast_node_t *node;
    
    switch (tok->type) {
        case TOK_INT_LIT:
            mcc_parser_advance(p);
            node = mcc_ast_create(p->ctx, AST_INT_LIT, tok->location);
            node->data.int_lit.value = tok->literal.int_val.value;
            node->data.int_lit.suffix = tok->literal.int_val.suffix;
            return node;
            
        case TOK_FLOAT_LIT:
            mcc_parser_advance(p);
            node = mcc_ast_create(p->ctx, AST_FLOAT_LIT, tok->location);
            node->data.float_lit.value = tok->literal.float_val.value;
            node->data.float_lit.suffix = tok->literal.float_val.suffix;
            return node;
            
        case TOK_CHAR_LIT:
            mcc_parser_advance(p);
            node = mcc_ast_create(p->ctx, AST_CHAR_LIT, tok->location);
            node->data.char_lit.value = tok->literal.char_val.value;
            return node;
            
        case TOK_STRING_LIT:
            mcc_parser_advance(p);
            node = mcc_ast_create(p->ctx, AST_STRING_LIT, tok->location);
            node->data.string_lit.value = tok->literal.string_val.value;
            node->data.string_lit.length = tok->literal.string_val.length;
            return node;
            
        case TOK_IDENT:
            mcc_parser_advance(p);
            node = mcc_ast_create(p->ctx, AST_IDENT_EXPR, tok->location);
            node->data.ident_expr.name = mcc_strdup(p->ctx, tok->text);
            return node;
            
        case TOK_LPAREN:
            mcc_parser_advance(p);
            
            /* Check for cast expression */
            if (is_type_start(p)) {
                mcc_type_t *type = parse_type_specifier(p);
                /* TODO: Parse abstract declarator */
                mcc_parser_expect(p, TOK_RPAREN, ")");
                
                mcc_ast_node_t *expr = parse_primary(p);
                node = mcc_ast_create(p->ctx, AST_CAST_EXPR, tok->location);
                node->data.cast_expr.target_type = type;
                node->data.cast_expr.expr = expr;
                return node;
            }
            
            node = parse_expression(p);
            mcc_parser_expect(p, TOK_RPAREN, ")");
            return node;
            
        case TOK_SIZEOF:
            mcc_parser_advance(p);
            node = mcc_ast_create(p->ctx, AST_SIZEOF_EXPR, tok->location);
            
            if (mcc_parser_match(p, TOK_LPAREN)) {
                if (is_type_start(p)) {
                    node->data.sizeof_expr.type_arg = parse_type_specifier(p);
                    /* TODO: Parse abstract declarator */
                } else {
                    node->data.sizeof_expr.expr_arg = parse_expression(p);
                }
                mcc_parser_expect(p, TOK_RPAREN, ")");
            } else {
                node->data.sizeof_expr.expr_arg = parse_primary(p);
            }
            return node;
            
        default:
            mcc_error_at(p->ctx, tok->location, "Expected expression, got '%s'",
                         mcc_token_to_string(tok));
            return NULL;
    }
}

/* Parse postfix expression */
static mcc_ast_node_t *parse_postfix(mcc_parser_t *p)
{
    mcc_ast_node_t *node = parse_primary(p);
    if (!node) return NULL;
    
    while (1) {
        mcc_token_t *tok = p->peek;
        
        if (mcc_parser_match(p, TOK_LBRACKET)) {
            /* Array subscript */
            mcc_ast_node_t *index = parse_expression(p);
            mcc_parser_expect(p, TOK_RBRACKET, "]");
            
            mcc_ast_node_t *subscript = mcc_ast_create(p->ctx, AST_SUBSCRIPT_EXPR, tok->location);
            subscript->data.subscript_expr.array = node;
            subscript->data.subscript_expr.index = index;
            node = subscript;
            
        } else if (mcc_parser_match(p, TOK_LPAREN)) {
            /* Function call */
            mcc_ast_node_t **args = NULL;
            size_t num_args = 0;
            size_t cap_args = 0;
            
            if (!mcc_parser_check(p, TOK_RPAREN)) {
                do {
                    mcc_ast_node_t *arg = parse_assignment_expr(p);
                    if (num_args >= cap_args) {
                        cap_args = cap_args ? cap_args * 2 : 4;
                        args = mcc_realloc(p->ctx, args,
                                           num_args * sizeof(mcc_ast_node_t*),
                                           cap_args * sizeof(mcc_ast_node_t*));
                    }
                    args[num_args++] = arg;
                } while (mcc_parser_match(p, TOK_COMMA));
            }
            mcc_parser_expect(p, TOK_RPAREN, ")");
            
            mcc_ast_node_t *call = mcc_ast_create(p->ctx, AST_CALL_EXPR, tok->location);
            call->data.call_expr.func = node;
            call->data.call_expr.args = args;
            call->data.call_expr.num_args = num_args;
            node = call;
            
        } else if (mcc_parser_match(p, TOK_DOT)) {
            /* Member access */
            mcc_token_t *member = mcc_parser_expect(p, TOK_IDENT, "member name");
            
            mcc_ast_node_t *mem = mcc_ast_create(p->ctx, AST_MEMBER_EXPR, tok->location);
            mem->data.member_expr.object = node;
            mem->data.member_expr.member = mcc_strdup(p->ctx, member->text);
            mem->data.member_expr.is_arrow = false;
            node = mem;
            
        } else if (mcc_parser_match(p, TOK_ARROW)) {
            /* Pointer member access */
            mcc_token_t *member = mcc_parser_expect(p, TOK_IDENT, "member name");
            
            mcc_ast_node_t *mem = mcc_ast_create(p->ctx, AST_MEMBER_EXPR, tok->location);
            mem->data.member_expr.object = node;
            mem->data.member_expr.member = mcc_strdup(p->ctx, member->text);
            mem->data.member_expr.is_arrow = true;
            node = mem;
            
        } else if (mcc_parser_match(p, TOK_INC)) {
            /* Post-increment */
            mcc_ast_node_t *inc = mcc_ast_create(p->ctx, AST_UNARY_EXPR, tok->location);
            inc->data.unary_expr.op = UNOP_POST_INC;
            inc->data.unary_expr.operand = node;
            node = inc;
            
        } else if (mcc_parser_match(p, TOK_DEC)) {
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

/* Parse unary expression */
static mcc_ast_node_t *parse_unary(mcc_parser_t *p)
{
    mcc_token_t *tok = p->peek;
    mcc_unop_t op;
    
    switch (tok->type) {
        case TOK_INC:
            mcc_parser_advance(p);
            op = UNOP_PRE_INC;
            break;
        case TOK_DEC:
            mcc_parser_advance(p);
            op = UNOP_PRE_DEC;
            break;
        case TOK_AMP:
            mcc_parser_advance(p);
            op = UNOP_ADDR;
            break;
        case TOK_STAR:
            mcc_parser_advance(p);
            op = UNOP_DEREF;
            break;
        case TOK_PLUS:
            mcc_parser_advance(p);
            op = UNOP_POS;
            break;
        case TOK_MINUS:
            mcc_parser_advance(p);
            op = UNOP_NEG;
            break;
        case TOK_TILDE:
            mcc_parser_advance(p);
            op = UNOP_BIT_NOT;
            break;
        case TOK_NOT:
            mcc_parser_advance(p);
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

/* Parse binary expression with precedence climbing */
static mcc_ast_node_t *parse_binary(mcc_parser_t *p, int min_prec)
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
        
        mcc_parser_advance(p);
        
        int next_prec = right_assoc ? prec : prec + 1;
        mcc_ast_node_t *right = parse_binary(p, next_prec);
        
        mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_BINARY_EXPR, tok->location);
        node->data.binary_expr.op = op;
        node->data.binary_expr.lhs = left;
        node->data.binary_expr.rhs = right;
        left = node;
    }
}

/* Parse ternary expression */
static mcc_ast_node_t *parse_ternary(mcc_parser_t *p)
{
    mcc_ast_node_t *cond = parse_binary(p, 1);
    if (!cond) return NULL;
    
    if (mcc_parser_match(p, TOK_QUESTION)) {
        mcc_location_t loc = p->current->location;
        mcc_ast_node_t *then_expr = parse_expression(p);
        mcc_parser_expect(p, TOK_COLON, ":");
        mcc_ast_node_t *else_expr = parse_ternary(p);
        
        mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_TERNARY_EXPR, loc);
        node->data.ternary_expr.cond = cond;
        node->data.ternary_expr.then_expr = then_expr;
        node->data.ternary_expr.else_expr = else_expr;
        return node;
    }
    
    return cond;
}

/* Parse assignment expression */
static mcc_ast_node_t *parse_assignment_expr(mcc_parser_t *p)
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
    
    mcc_parser_advance(p);
    mcc_ast_node_t *right = parse_assignment_expr(p);
    
    mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_BINARY_EXPR, tok->location);
    node->data.binary_expr.op = op;
    node->data.binary_expr.lhs = left;
    node->data.binary_expr.rhs = right;
    return node;
}

/* Parse expression (comma expression) */
static mcc_ast_node_t *parse_expression(mcc_parser_t *p)
{
    mcc_ast_node_t *left = parse_assignment_expr(p);
    if (!left) return NULL;
    
    while (mcc_parser_match(p, TOK_COMMA)) {
        mcc_location_t loc = p->current->location;
        mcc_ast_node_t *right = parse_assignment_expr(p);
        
        mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_COMMA_EXPR, loc);
        node->data.comma_expr.left = left;
        node->data.comma_expr.right = right;
        left = node;
    }
    
    return left;
}

/* Check if identifier is a typedef name */
static bool is_typedef_name(mcc_parser_t *p, const char *name)
{
    for (mcc_typedef_entry_t *t = p->typedefs; t; t = t->next) {
        if (strcmp(t->name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Parse type specifier (simplified) */
static mcc_type_t *parse_type_specifier(mcc_parser_t *p)
{
    /* This is a simplified type parser */
    /* A full implementation would handle all C89 type specifiers */
    
    bool is_unsigned = false;
    bool is_signed = false;
    bool is_const = false;
    bool is_volatile = false;
    int type_spec = 0; /* 0=none, 1=void, 2=char, 3=short, 4=int, 5=long, 6=float, 7=double */
    
    while (1) {
        switch (p->peek->type) {
            case TOK_CONST:
                mcc_parser_advance(p);
                is_const = true;
                continue;
            case TOK_VOLATILE:
                mcc_parser_advance(p);
                is_volatile = true;
                continue;
            case TOK_UNSIGNED:
                mcc_parser_advance(p);
                is_unsigned = true;
                continue;
            case TOK_SIGNED:
                mcc_parser_advance(p);
                is_signed = true;
                continue;
            case TOK_VOID:
                mcc_parser_advance(p);
                type_spec = 1;
                break;
            case TOK_CHAR:
                mcc_parser_advance(p);
                type_spec = 2;
                break;
            case TOK_SHORT:
                mcc_parser_advance(p);
                type_spec = 3;
                break;
            case TOK_INT:
                mcc_parser_advance(p);
                type_spec = 4;
                break;
            case TOK_LONG:
                mcc_parser_advance(p);
                type_spec = 5;
                break;
            case TOK_FLOAT:
                mcc_parser_advance(p);
                type_spec = 6;
                break;
            case TOK_DOUBLE:
                mcc_parser_advance(p);
                type_spec = 7;
                break;
            case TOK_STRUCT:
            case TOK_UNION: {
                bool is_union = (p->peek->type == TOK_UNION);
                mcc_parser_advance(p);
                
                /* Get tag name if present */
                const char *tag = NULL;
                if (mcc_parser_check(p, TOK_IDENT)) {
                    tag = mcc_strdup(p->ctx, p->peek->text);
                    mcc_parser_advance(p);
                }
                
                mcc_type_t *stype = NULL;
                
                /* Check if this is a definition or reference */
                if (mcc_parser_match(p, TOK_LBRACE)) {
                    /* Definition - create new type */
                    stype = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                    stype->kind = is_union ? TYPE_UNION : TYPE_STRUCT;
                    stype->data.record.tag = tag;
                    stype->data.record.fields = NULL;
                    stype->data.record.num_fields = 0;
                    
                    /* Parse fields */
                    mcc_struct_field_t *fields = NULL;
                    mcc_struct_field_t *field_tail = NULL;
                    int num_fields = 0;
                    
                    while (!mcc_parser_check(p, TOK_RBRACE) && !mcc_parser_check(p, TOK_EOF)) {
                        mcc_type_t *field_base_type = parse_type_specifier(p);
                        
                        /* Parse field declarator(s) */
                        do {
                            /* Parse pointer(s) for this field */
                            mcc_type_t *field_type = field_base_type;
                            while (mcc_parser_match(p, TOK_STAR)) {
                                mcc_type_t *ptr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                                ptr->kind = TYPE_POINTER;
                                ptr->data.pointer.pointee = field_type;
                                field_type = ptr;
                            }
                            
                            mcc_token_t *field_name = mcc_parser_expect(p, TOK_IDENT, "field name");
                            
                            /* Parse array brackets for this field */
                            while (mcc_parser_match(p, TOK_LBRACKET)) {
                                size_t arr_size = 0;
                                if (!mcc_parser_check(p, TOK_RBRACKET)) {
                                    mcc_ast_node_t *size_expr = parse_expression(p);
                                    if (size_expr && size_expr->kind == AST_INT_LIT) {
                                        arr_size = size_expr->data.int_lit.value;
                                    }
                                }
                                mcc_parser_expect(p, TOK_RBRACKET, "]");
                                
                                mcc_type_t *arr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                                arr->kind = TYPE_ARRAY;
                                arr->data.array.element = field_type;
                                arr->data.array.length = arr_size;
                                field_type = arr;
                            }
                            
                            mcc_struct_field_t *field = mcc_alloc(p->ctx, sizeof(mcc_struct_field_t));
                            field->name = mcc_strdup(p->ctx, field_name->text);
                            field->type = field_type;
                            field->next = NULL;
                            
                            if (!fields) fields = field;
                            if (field_tail) field_tail->next = field;
                            field_tail = field;
                            num_fields++;
                            
                        } while (mcc_parser_match(p, TOK_COMMA));
                        
                        mcc_parser_expect(p, TOK_SEMICOLON, ";");
                    }
                    
                    mcc_parser_expect(p, TOK_RBRACE, "}");
                    
                    stype->data.record.fields = fields;
                    stype->data.record.num_fields = num_fields;
                    
                    /* Register in struct table if tagged */
                    if (tag) {
                        mcc_struct_entry_t *entry = mcc_alloc(p->ctx, sizeof(mcc_struct_entry_t));
                        entry->tag = tag;
                        entry->type = stype;
                        entry->next = p->struct_types;
                        p->struct_types = entry;
                    }
                } else if (tag) {
                    /* Reference - look up existing type */
                    for (mcc_struct_entry_t *e = p->struct_types; e; e = e->next) {
                        if (strcmp(e->tag, tag) == 0) {
                            stype = e->type;
                            break;
                        }
                    }
                    
                    if (!stype) {
                        /* Forward declaration - create incomplete type */
                        stype = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                        stype->kind = is_union ? TYPE_UNION : TYPE_STRUCT;
                        stype->data.record.tag = tag;
                        stype->data.record.fields = NULL;
                        stype->data.record.num_fields = 0;
                        
                        /* Register for later completion */
                        mcc_struct_entry_t *entry = mcc_alloc(p->ctx, sizeof(mcc_struct_entry_t));
                        entry->tag = tag;
                        entry->type = stype;
                        entry->next = p->struct_types;
                        p->struct_types = entry;
                    }
                } else {
                    /* Anonymous struct without definition - error */
                    mcc_error(p->ctx, "Anonymous struct/union must have a definition");
                    stype = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                    stype->kind = TYPE_INT;
                }
                
                /* Handle qualifiers and pointers */
                if (is_const) stype->qualifiers |= QUAL_CONST;
                if (is_volatile) stype->qualifiers |= QUAL_VOLATILE;
                
                /* Parse pointer */
                while (mcc_parser_match(p, TOK_STAR)) {
                    mcc_type_t *ptr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                    ptr->kind = TYPE_POINTER;
                    ptr->data.pointer.pointee = stype;
                    stype = ptr;
                }
                
                return stype;
            }
            default:
                /* Check for typedef name */
                if (p->peek->type == TOK_IDENT && type_spec == 0) {
                    for (mcc_typedef_entry_t *t = p->typedefs; t; t = t->next) {
                        if (strcmp(t->name, p->peek->text) == 0) {
                            mcc_parser_advance(p);
                            mcc_type_t *type = t->type;
                            
                            /* Handle qualifiers */
                            if (is_const) type->qualifiers |= QUAL_CONST;
                            if (is_volatile) type->qualifiers |= QUAL_VOLATILE;
                            
                            /* Parse pointer */
                            while (mcc_parser_match(p, TOK_STAR)) {
                                mcc_type_t *ptr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                                ptr->kind = TYPE_POINTER;
                                ptr->data.pointer.pointee = type;
                                type = ptr;
                            }
                            
                            return type;
                        }
                    }
                }
                goto done;
        }
        break;
    }
done:
    
    /* Default to int if only signed/unsigned */
    if (type_spec == 0 && (is_signed || is_unsigned)) {
        type_spec = 4; /* int */
    }
    
    /* Create type (placeholder - actual type creation would use type context) */
    mcc_type_t *type = mcc_alloc(p->ctx, sizeof(mcc_type_t));
    
    switch (type_spec) {
        case 1: type->kind = TYPE_VOID; break;
        case 2: type->kind = TYPE_CHAR; break;
        case 3: type->kind = TYPE_SHORT; break;
        case 4: type->kind = TYPE_INT; break;
        case 5: type->kind = TYPE_LONG; break;
        case 6: type->kind = TYPE_FLOAT; break;
        case 7: type->kind = TYPE_DOUBLE; break;
        default: type->kind = TYPE_INT; break;
    }
    
    type->is_unsigned = is_unsigned;
    if (is_const) type->qualifiers |= QUAL_CONST;
    if (is_volatile) type->qualifiers |= QUAL_VOLATILE;
    
    /* Parse pointer */
    while (mcc_parser_match(p, TOK_STAR)) {
        mcc_type_t *ptr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
        ptr->kind = TYPE_POINTER;
        ptr->data.pointer.pointee = type;
        type = ptr;
        
        /* Pointer qualifiers */
        while (1) {
            if (mcc_parser_match(p, TOK_CONST)) {
                type->qualifiers |= QUAL_CONST;
            } else if (mcc_parser_match(p, TOK_VOLATILE)) {
                type->qualifiers |= QUAL_VOLATILE;
            } else {
                break;
            }
        }
    }
    
    return type;
}

/* Parse compound statement */
static mcc_ast_node_t *parse_compound_stmt(mcc_parser_t *p)
{
    mcc_location_t loc = p->peek->location;
    mcc_parser_expect(p, TOK_LBRACE, "{");
    
    mcc_ast_node_t **stmts = NULL;
    size_t num_stmts = 0;
    size_t cap_stmts = 0;
    
    while (!mcc_parser_check(p, TOK_RBRACE) && !mcc_parser_check(p, TOK_EOF)) {
        mcc_ast_node_t *stmt;
        
        if (is_declaration_start(p)) {
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
            mcc_parser_synchronize(p);
        }
    }
    
    mcc_parser_expect(p, TOK_RBRACE, "}");
    
    mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_COMPOUND_STMT, loc);
    node->data.compound_stmt.stmts = stmts;
    node->data.compound_stmt.num_stmts = num_stmts;
    return node;
}

/* Parse statement */
static mcc_ast_node_t *parse_statement(mcc_parser_t *p)
{
    mcc_token_t *tok = p->peek;
    mcc_ast_node_t *node;
    
    switch (tok->type) {
        case TOK_LBRACE:
            return parse_compound_stmt(p);
            
        case TOK_IF: {
            mcc_parser_advance(p);
            mcc_parser_expect(p, TOK_LPAREN, "(");
            mcc_ast_node_t *cond = parse_expression(p);
            mcc_parser_expect(p, TOK_RPAREN, ")");
            mcc_ast_node_t *then_stmt = parse_statement(p);
            mcc_ast_node_t *else_stmt = NULL;
            if (mcc_parser_match(p, TOK_ELSE)) {
                else_stmt = parse_statement(p);
            }
            
            node = mcc_ast_create(p->ctx, AST_IF_STMT, tok->location);
            node->data.if_stmt.cond = cond;
            node->data.if_stmt.then_stmt = then_stmt;
            node->data.if_stmt.else_stmt = else_stmt;
            return node;
        }
        
        case TOK_WHILE: {
            mcc_parser_advance(p);
            mcc_parser_expect(p, TOK_LPAREN, "(");
            mcc_ast_node_t *cond = parse_expression(p);
            mcc_parser_expect(p, TOK_RPAREN, ")");
            mcc_ast_node_t *body = parse_statement(p);
            
            node = mcc_ast_create(p->ctx, AST_WHILE_STMT, tok->location);
            node->data.while_stmt.cond = cond;
            node->data.while_stmt.body = body;
            return node;
        }
        
        case TOK_DO: {
            mcc_parser_advance(p);
            mcc_ast_node_t *body = parse_statement(p);
            mcc_parser_expect(p, TOK_WHILE, "while");
            mcc_parser_expect(p, TOK_LPAREN, "(");
            mcc_ast_node_t *cond = parse_expression(p);
            mcc_parser_expect(p, TOK_RPAREN, ")");
            mcc_parser_expect(p, TOK_SEMICOLON, ";");
            
            node = mcc_ast_create(p->ctx, AST_DO_STMT, tok->location);
            node->data.do_stmt.body = body;
            node->data.do_stmt.cond = cond;
            return node;
        }
        
        case TOK_FOR: {
            mcc_parser_advance(p);
            mcc_parser_expect(p, TOK_LPAREN, "(");
            
            mcc_ast_node_t *init = NULL;
            if (!mcc_parser_check(p, TOK_SEMICOLON)) {
                init = parse_expression(p);
            }
            mcc_parser_expect(p, TOK_SEMICOLON, ";");
            
            mcc_ast_node_t *cond = NULL;
            if (!mcc_parser_check(p, TOK_SEMICOLON)) {
                cond = parse_expression(p);
            }
            mcc_parser_expect(p, TOK_SEMICOLON, ";");
            
            mcc_ast_node_t *incr = NULL;
            if (!mcc_parser_check(p, TOK_RPAREN)) {
                incr = parse_expression(p);
            }
            mcc_parser_expect(p, TOK_RPAREN, ")");
            
            mcc_ast_node_t *body = parse_statement(p);
            
            node = mcc_ast_create(p->ctx, AST_FOR_STMT, tok->location);
            node->data.for_stmt.init = init;
            node->data.for_stmt.cond = cond;
            node->data.for_stmt.incr = incr;
            node->data.for_stmt.body = body;
            return node;
        }
        
        case TOK_SWITCH: {
            mcc_parser_advance(p);
            mcc_parser_expect(p, TOK_LPAREN, "(");
            mcc_ast_node_t *expr = parse_expression(p);
            mcc_parser_expect(p, TOK_RPAREN, ")");
            mcc_ast_node_t *body = parse_statement(p);
            
            node = mcc_ast_create(p->ctx, AST_SWITCH_STMT, tok->location);
            node->data.switch_stmt.expr = expr;
            node->data.switch_stmt.body = body;
            return node;
        }
        
        case TOK_CASE: {
            mcc_parser_advance(p);
            mcc_ast_node_t *expr = parse_expression(p);
            mcc_parser_expect(p, TOK_COLON, ":");
            mcc_ast_node_t *stmt = parse_statement(p);
            
            node = mcc_ast_create(p->ctx, AST_CASE_STMT, tok->location);
            node->data.case_stmt.expr = expr;
            node->data.case_stmt.stmt = stmt;
            return node;
        }
        
        case TOK_DEFAULT: {
            mcc_parser_advance(p);
            mcc_parser_expect(p, TOK_COLON, ":");
            mcc_ast_node_t *stmt = parse_statement(p);
            
            node = mcc_ast_create(p->ctx, AST_DEFAULT_STMT, tok->location);
            node->data.default_stmt.stmt = stmt;
            return node;
        }
        
        case TOK_BREAK:
            mcc_parser_advance(p);
            mcc_parser_expect(p, TOK_SEMICOLON, ";");
            return mcc_ast_create(p->ctx, AST_BREAK_STMT, tok->location);
            
        case TOK_CONTINUE:
            mcc_parser_advance(p);
            mcc_parser_expect(p, TOK_SEMICOLON, ";");
            return mcc_ast_create(p->ctx, AST_CONTINUE_STMT, tok->location);
            
        case TOK_RETURN: {
            mcc_parser_advance(p);
            mcc_ast_node_t *expr = NULL;
            if (!mcc_parser_check(p, TOK_SEMICOLON)) {
                expr = parse_expression(p);
            }
            mcc_parser_expect(p, TOK_SEMICOLON, ";");
            
            node = mcc_ast_create(p->ctx, AST_RETURN_STMT, tok->location);
            node->data.return_stmt.expr = expr;
            return node;
        }
        
        case TOK_GOTO: {
            mcc_parser_advance(p);
            mcc_token_t *label = mcc_parser_expect(p, TOK_IDENT, "label");
            mcc_parser_expect(p, TOK_SEMICOLON, ";");
            
            node = mcc_ast_create(p->ctx, AST_GOTO_STMT, tok->location);
            node->data.goto_stmt.label = mcc_strdup(p->ctx, label->text);
            return node;
        }
        
        case TOK_SEMICOLON:
            mcc_parser_advance(p);
            return mcc_ast_create(p->ctx, AST_NULL_STMT, tok->location);
            
        case TOK_IDENT: {
            /* Check if it's a typedef name - then it's a declaration */
            if (is_typedef_name(p, p->peek->text)) {
                return parse_declaration(p);
            }
            
            /* Check for label - need to look ahead */
            mcc_token_t *ident_tok = p->peek;
            mcc_parser_advance(p);
            
            if (mcc_parser_check(p, TOK_COLON)) {
                /* It's a label */
                mcc_parser_advance(p);
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
                if (mcc_parser_match(p, TOK_DOT)) {
                    mcc_token_t *member = mcc_parser_expect(p, TOK_IDENT, "member name");
                    mcc_ast_node_t *mem = mcc_ast_create(p->ctx, AST_MEMBER_EXPR, ident_tok->location);
                    mem->data.member_expr.object = expr;
                    mem->data.member_expr.member = mcc_strdup(p->ctx, member->text);
                    mem->data.member_expr.is_arrow = false;
                    expr = mem;
                } else if (mcc_parser_match(p, TOK_ARROW)) {
                    mcc_token_t *member = mcc_parser_expect(p, TOK_IDENT, "member name");
                    mcc_ast_node_t *mem = mcc_ast_create(p->ctx, AST_MEMBER_EXPR, ident_tok->location);
                    mem->data.member_expr.object = expr;
                    mem->data.member_expr.member = mcc_strdup(p->ctx, member->text);
                    mem->data.member_expr.is_arrow = true;
                    expr = mem;
                } else if (mcc_parser_match(p, TOK_LBRACKET)) {
                    mcc_ast_node_t *index = parse_expression(p);
                    mcc_parser_expect(p, TOK_RBRACKET, "]");
                    mcc_ast_node_t *sub = mcc_ast_create(p->ctx, AST_SUBSCRIPT_EXPR, ident_tok->location);
                    sub->data.subscript_expr.array = expr;
                    sub->data.subscript_expr.index = index;
                    expr = sub;
                } else if (mcc_parser_match(p, TOK_LPAREN)) {
                    /* Function call */
                    mcc_ast_node_t **args = NULL;
                    size_t num_args = 0;
                    size_t cap_args = 0;
                    
                    if (!mcc_parser_check(p, TOK_RPAREN)) {
                        do {
                            mcc_ast_node_t *arg = parse_assignment_expr(p);
                            if (num_args >= cap_args) {
                                cap_args = cap_args ? cap_args * 2 : 4;
                                args = mcc_realloc(p->ctx, args,
                                                   num_args * sizeof(mcc_ast_node_t*),
                                                   cap_args * sizeof(mcc_ast_node_t*));
                            }
                            args[num_args++] = arg;
                        } while (mcc_parser_match(p, TOK_COMMA));
                    }
                    mcc_parser_expect(p, TOK_RPAREN, ")");
                    
                    mcc_ast_node_t *call = mcc_ast_create(p->ctx, AST_CALL_EXPR, ident_tok->location);
                    call->data.call_expr.func = expr;
                    call->data.call_expr.args = args;
                    call->data.call_expr.num_args = num_args;
                    expr = call;
                } else {
                    break;
                }
            }
            
            /* Handle assignment */
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
                mcc_parser_advance(p);
                mcc_ast_node_t *rhs = parse_assignment_expr(p);
                mcc_ast_node_t *bin = mcc_ast_create(p->ctx, AST_BINARY_EXPR, op_tok->location);
                bin->data.binary_expr.op = op;
                bin->data.binary_expr.lhs = expr;
                bin->data.binary_expr.rhs = rhs;
                expr = bin;
            }
            
            mcc_parser_expect(p, TOK_SEMICOLON, ";");
            
            node = mcc_ast_create(p->ctx, AST_EXPR_STMT, ident_tok->location);
            node->data.expr_stmt.expr = expr;
            return node;
        }
            
        default: {
            /* Expression statement */
            mcc_ast_node_t *expr = parse_expression(p);
            mcc_parser_expect(p, TOK_SEMICOLON, ";");
            
            node = mcc_ast_create(p->ctx, AST_EXPR_STMT, tok->location);
            node->data.expr_stmt.expr = expr;
            return node;
        }
    }
}

/* Parse declaration */
static mcc_ast_node_t *parse_declaration(mcc_parser_t *p)
{
    mcc_location_t loc = p->peek->location;
    
    /* Parse storage class */
    mcc_storage_class_t storage = STORAGE_NONE;
    bool is_typedef = false;
    
    while (1) {
        switch (p->peek->type) {
            case TOK_TYPEDEF:
                mcc_parser_advance(p);
                is_typedef = true;
                storage = STORAGE_TYPEDEF;
                continue;
            case TOK_EXTERN:
                mcc_parser_advance(p);
                storage = STORAGE_EXTERN;
                continue;
            case TOK_STATIC:
                mcc_parser_advance(p);
                storage = STORAGE_STATIC;
                continue;
            case TOK_AUTO:
                mcc_parser_advance(p);
                storage = STORAGE_AUTO;
                continue;
            case TOK_REGISTER:
                mcc_parser_advance(p);
                storage = STORAGE_REGISTER;
                continue;
            default:
                break;
        }
        break;
    }
    
    /* Parse type specifier */
    mcc_type_t *base_type = parse_type_specifier(p);
    
    /* Parse declarator(s) */
    if (mcc_parser_check(p, TOK_SEMICOLON)) {
        /* Type declaration only (e.g., struct definition) */
        mcc_parser_advance(p);
        return NULL; /* TODO: Return struct/union/enum declaration */
    }
    
    /* Get declarator name */
    mcc_token_t *name_tok = mcc_parser_expect(p, TOK_IDENT, "identifier");
    const char *name = mcc_strdup(p->ctx, name_tok->text);
    
    /* Check for function declaration */
    if (mcc_parser_check(p, TOK_LPAREN)) {
        mcc_parser_advance(p);
        
        /* Parse parameters */
        mcc_ast_node_t **params = NULL;
        size_t num_params = 0;
        size_t cap_params = 0;
        
        if (!mcc_parser_check(p, TOK_RPAREN)) {
            /* Check for void parameter */
            if (mcc_parser_check(p, TOK_VOID)) {
                mcc_parser_advance(p);
                if (mcc_parser_check(p, TOK_RPAREN)) {
                    /* void means no parameters */
                } else {
                    /* void* or similar */
                    /* TODO: Handle properly */
                }
            } else {
                do {
                    mcc_type_t *param_type = parse_type_specifier(p);
                    const char *param_name = NULL;
                    
                    if (mcc_parser_check(p, TOK_IDENT)) {
                        param_name = mcc_strdup(p->ctx, p->peek->text);
                        mcc_parser_advance(p);
                    }
                    
                    /* Parse array brackets */
                    while (mcc_parser_match(p, TOK_LBRACKET)) {
                        /* TODO: Parse array size */
                        if (!mcc_parser_check(p, TOK_RBRACKET)) {
                            parse_expression(p);
                        }
                        mcc_parser_expect(p, TOK_RBRACKET, "]");
                        
                        mcc_type_t *ptr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                        ptr->kind = TYPE_POINTER;
                        ptr->data.pointer.pointee = param_type;
                        param_type = ptr;
                    }
                    
                    mcc_ast_node_t *param = mcc_ast_create(p->ctx, AST_PARAM_DECL, loc);
                    param->data.param_decl.name = param_name;
                    param->data.param_decl.param_type = param_type;
                    
                    if (num_params >= cap_params) {
                        cap_params = cap_params ? cap_params * 2 : 4;
                        params = mcc_realloc(p->ctx, params,
                                             num_params * sizeof(mcc_ast_node_t*),
                                             cap_params * sizeof(mcc_ast_node_t*));
                    }
                    params[num_params++] = param;
                    
                } while (mcc_parser_match(p, TOK_COMMA) && !mcc_parser_check(p, TOK_ELLIPSIS));
                
                if (mcc_parser_match(p, TOK_ELLIPSIS)) {
                    /* Variadic function */
                    /* TODO: Mark as variadic */
                }
            }
        }
        
        mcc_parser_expect(p, TOK_RPAREN, ")");
        
        /* Check for function body or declaration */
        mcc_ast_node_t *body = NULL;
        bool is_definition = false;
        
        if (mcc_parser_check(p, TOK_LBRACE)) {
            body = parse_compound_stmt(p);
            is_definition = true;
        } else {
            mcc_parser_expect(p, TOK_SEMICOLON, ";");
        }
        
        mcc_ast_node_t *func = mcc_ast_create(p->ctx, AST_FUNC_DECL, loc);
        func->data.func_decl.name = name;
        func->data.func_decl.func_type = base_type;
        func->data.func_decl.params = params;
        func->data.func_decl.num_params = num_params;
        func->data.func_decl.body = body;
        func->data.func_decl.is_definition = is_definition;
        func->data.func_decl.is_static = (storage == STORAGE_STATIC);
        return func;
    }
    
    /* Variable/typedef declaration - handle multiple declarators */
    /* First declarator already has name parsed, now handle the rest */
    
    /* Build type for first declarator (may have pointer from type_specifier) */
    mcc_type_t *decl_type = base_type;
    
    /* Parse array brackets for first declarator */
    while (mcc_parser_match(p, TOK_LBRACKET)) {
        size_t array_size = 0;
        if (!mcc_parser_check(p, TOK_RBRACKET)) {
            mcc_ast_node_t *size_expr = parse_expression(p);
            if (size_expr && size_expr->kind == AST_INT_LIT) {
                array_size = size_expr->data.int_lit.value;
            }
        }
        mcc_parser_expect(p, TOK_RBRACKET, "]");
        
        mcc_type_t *arr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
        arr->kind = TYPE_ARRAY;
        arr->data.array.element = decl_type;
        arr->data.array.length = array_size;
        decl_type = arr;
    }
    
    /* Handle typedef - may have multiple names */
    if (is_typedef) {
        /* Register first typedef name */
        mcc_typedef_entry_t *entry = mcc_alloc(p->ctx, sizeof(mcc_typedef_entry_t));
        entry->name = name;
        entry->type = decl_type;
        entry->next = p->typedefs;
        p->typedefs = entry;
        
        /* Parse additional typedef names separated by comma */
        while (mcc_parser_match(p, TOK_COMMA)) {
            /* Parse pointer(s) for this declarator */
            mcc_type_t *next_type = base_type;
            while (mcc_parser_match(p, TOK_STAR)) {
                mcc_type_t *ptr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                ptr->kind = TYPE_POINTER;
                ptr->data.pointer.pointee = next_type;
                next_type = ptr;
            }
            
            /* Get next name */
            mcc_token_t *next_tok = mcc_parser_expect(p, TOK_IDENT, "typedef name");
            const char *next_name = mcc_strdup(p->ctx, next_tok->text);
            
            /* Parse array brackets */
            while (mcc_parser_match(p, TOK_LBRACKET)) {
                size_t arr_size = 0;
                if (!mcc_parser_check(p, TOK_RBRACKET)) {
                    mcc_ast_node_t *size_expr = parse_expression(p);
                    if (size_expr && size_expr->kind == AST_INT_LIT) {
                        arr_size = size_expr->data.int_lit.value;
                    }
                }
                mcc_parser_expect(p, TOK_RBRACKET, "]");
                
                mcc_type_t *arr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                arr->kind = TYPE_ARRAY;
                arr->data.array.element = next_type;
                arr->data.array.length = arr_size;
                next_type = arr;
            }
            
            /* Register this typedef */
            mcc_typedef_entry_t *next_entry = mcc_alloc(p->ctx, sizeof(mcc_typedef_entry_t));
            next_entry->name = next_name;
            next_entry->type = next_type;
            next_entry->next = p->typedefs;
            p->typedefs = next_entry;
        }
        
        mcc_parser_expect(p, TOK_SEMICOLON, ";");
        
        /* Return a typedef declaration node for the first name */
        mcc_ast_node_t *td = mcc_ast_create(p->ctx, AST_TYPEDEF_DECL, loc);
        td->data.typedef_decl.name = name;
        td->data.typedef_decl.type = decl_type;
        return td;
    }
    
    /* Parse initializer for variable */
    mcc_ast_node_t *init = NULL;
    if (mcc_parser_match(p, TOK_ASSIGN)) {
        if (mcc_parser_check(p, TOK_LBRACE)) {
            /* Initializer list */
            mcc_parser_advance(p);
            
            mcc_ast_node_t **exprs = NULL;
            size_t num_exprs = 0;
            size_t cap_exprs = 0;
            
            if (!mcc_parser_check(p, TOK_RBRACE)) {
                do {
                    mcc_ast_node_t *expr = parse_assignment_expr(p);
                    if (num_exprs >= cap_exprs) {
                        cap_exprs = cap_exprs ? cap_exprs * 2 : 4;
                        exprs = mcc_realloc(p->ctx, exprs,
                                            num_exprs * sizeof(mcc_ast_node_t*),
                                            cap_exprs * sizeof(mcc_ast_node_t*));
                    }
                    exprs[num_exprs++] = expr;
                } while (mcc_parser_match(p, TOK_COMMA) && !mcc_parser_check(p, TOK_RBRACE));
            }
            
            mcc_parser_expect(p, TOK_RBRACE, "}");
            
            init = mcc_ast_create(p->ctx, AST_INIT_LIST, loc);
            init->data.init_list.exprs = exprs;
            init->data.init_list.num_exprs = num_exprs;
        } else {
            init = parse_assignment_expr(p);
        }
    }
    
    mcc_parser_expect(p, TOK_SEMICOLON, ";");
    
    mcc_ast_node_t *var = mcc_ast_create(p->ctx, AST_VAR_DECL, loc);
    var->data.var_decl.name = name;
    var->data.var_decl.var_type = decl_type;
    var->data.var_decl.init = init;
    var->data.var_decl.is_static = (storage == STORAGE_STATIC);
    var->data.var_decl.is_extern = (storage == STORAGE_EXTERN);
    return var;
}

/* Main parsing entry point */
mcc_ast_node_t *mcc_parser_parse(mcc_parser_t *p)
{
    /* Initialize token stream */
    p->peek = mcc_preprocessor_next(p->pp);
    while (p->peek->type == TOK_NEWLINE) {
        p->peek = mcc_preprocessor_next(p->pp);
    }
    
    mcc_ast_node_t **decls = NULL;
    size_t num_decls = 0;
    size_t cap_decls = 0;
    
    while (!mcc_parser_check(p, TOK_EOF)) {
        mcc_ast_node_t *decl = parse_declaration(p);
        
        if (decl) {
            if (num_decls >= cap_decls) {
                cap_decls = cap_decls ? cap_decls * 2 : 8;
                decls = mcc_realloc(p->ctx, decls,
                                    num_decls * sizeof(mcc_ast_node_t*),
                                    cap_decls * sizeof(mcc_ast_node_t*));
            }
            decls[num_decls++] = decl;
        }
        
        if (p->panic_mode) {
            mcc_parser_synchronize(p);
        }
    }
    
    mcc_ast_node_t *tu = mcc_ast_create(p->ctx, AST_TRANSLATION_UNIT, (mcc_location_t){NULL, 0, 0});
    tu->data.translation_unit.decls = decls;
    tu->data.translation_unit.num_decls = num_decls;
    return tu;
}

mcc_ast_node_t *mcc_parser_parse_expression(mcc_parser_t *p)
{
    p->peek = mcc_preprocessor_next(p->pp);
    return parse_expression(p);
}

mcc_ast_node_t *mcc_parser_parse_statement(mcc_parser_t *p)
{
    p->peek = mcc_preprocessor_next(p->pp);
    return parse_statement(p);
}

mcc_ast_node_t *mcc_parser_parse_declaration(mcc_parser_t *p)
{
    p->peek = mcc_preprocessor_next(p->pp);
    return parse_declaration(p);
}
