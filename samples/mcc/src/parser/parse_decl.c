/*
 * MCC - Micro C Compiler
 * Declaration Parser
 * 
 * This file handles parsing of C declarations:
 * - Variable declarations
 * - Function declarations/definitions
 * - Typedef declarations
 * - Struct/union/enum declarations
 * - Initializers
 */

#include "parse_internal.h"

/* ============================================================
 * Initializer Parsing
 * ============================================================ */

mcc_ast_node_t *parse_initializer(mcc_parser_t *p)
{
    mcc_location_t loc = p->peek->location;
    
    if (parse_match(p, TOK_LBRACE)) {
        /* Initializer list { ... } */
        mcc_ast_node_t **exprs = NULL;
        size_t num_exprs = 0;
        size_t cap_exprs = 0;
        
        if (!parse_check(p, TOK_RBRACE)) {
            do {
                mcc_ast_node_t *expr = NULL;
                
                /* C99: Designated initializers */
                if (parse_check(p, TOK_DOT) || parse_check(p, TOK_LBRACKET)) {
                    if (!parse_has_designated_init(p)) {
                        mcc_warning_at(p->ctx, p->peek->location,
                            "designated initializers are a C99 extension");
                    }
                    
                    /* Parse designator list */
                    mcc_ast_node_t *designator = NULL;
                    while (parse_check(p, TOK_DOT) || parse_check(p, TOK_LBRACKET)) {
                        if (parse_match(p, TOK_DOT)) {
                            mcc_token_t *field = parse_expect(p, TOK_IDENT, "field name");
                            mcc_ast_node_t *des = mcc_ast_create(p->ctx, AST_FIELD_DESIGNATOR, field->location);
                            des->data.field_designator.name = mcc_strdup(p->ctx, field->text);
                            des->data.field_designator.next = designator;
                            designator = des;
                        } else if (parse_match(p, TOK_LBRACKET)) {
                            mcc_ast_node_t *index = parse_constant_expr(p);
                            parse_expect(p, TOK_RBRACKET, "]");
                            mcc_ast_node_t *des = mcc_ast_create(p->ctx, AST_INDEX_DESIGNATOR, loc);
                            des->data.index_designator.index = index;
                            des->data.index_designator.next = designator;
                            designator = des;
                        }
                    }
                    parse_expect(p, TOK_ASSIGN, "=");
                    
                    /* Parse initializer value */
                    mcc_ast_node_t *value = parse_initializer(p);
                    
                    expr = mcc_ast_create(p->ctx, AST_DESIGNATED_INIT, loc);
                    expr->data.designated_init.designator = designator;
                    expr->data.designated_init.value = value;
                } else {
                    /* Regular initializer */
                    expr = parse_initializer(p);
                }
                
                if (num_exprs >= cap_exprs) {
                    cap_exprs = cap_exprs ? cap_exprs * 2 : 4;
                    exprs = mcc_realloc(p->ctx, exprs,
                                        num_exprs * sizeof(mcc_ast_node_t*),
                                        cap_exprs * sizeof(mcc_ast_node_t*));
                }
                exprs[num_exprs++] = expr;
                
            } while (parse_match(p, TOK_COMMA) && !parse_check(p, TOK_RBRACE));
        }
        
        parse_expect(p, TOK_RBRACE, "}");
        
        mcc_ast_node_t *init = mcc_ast_create(p->ctx, AST_INIT_LIST, loc);
        init->data.init_list.exprs = exprs;
        init->data.init_list.num_exprs = num_exprs;
        return init;
    } else {
        /* Single expression initializer */
        return parse_assignment_expr(p);
    }
}

/* ============================================================
 * Function Declaration/Definition
 * ============================================================ */

mcc_ast_node_t *parse_function_decl(mcc_parser_t *p, mcc_type_t *base_type,
                                     const char *name, mcc_storage_class_t storage,
                                     mcc_location_t loc)
{
    parse_advance(p); /* consume '(' */
    
    /* Parse parameters */
    mcc_ast_node_t **params = NULL;
    size_t num_params = 0;
    size_t cap_params = 0;
    bool is_variadic = false;
    
    if (!parse_check(p, TOK_RPAREN)) {
        /* Check for void parameter */
        if (parse_check(p, TOK_VOID)) {
            mcc_token_t *void_tok = p->peek;
            parse_advance(p);
            if (parse_check(p, TOK_RPAREN)) {
                /* void means no parameters */
                goto end_params;
            } else {
                /* void* or similar - need to handle as type */
                /* Put back and parse as regular parameter */
                /* This is a simplification - proper handling would need lookahead */
                mcc_type_t *param_type = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                param_type->kind = TYPE_VOID;
                
                /* Parse pointer */
                while (parse_match(p, TOK_STAR)) {
                    mcc_type_t *ptr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                    ptr->kind = TYPE_POINTER;
                    ptr->data.pointer.pointee = param_type;
                    param_type = ptr;
                }
                
                const char *param_name = NULL;
                if (parse_check(p, TOK_IDENT)) {
                    param_name = mcc_strdup(p->ctx, p->peek->text);
                    parse_advance(p);
                }
                
                mcc_ast_node_t *param = mcc_ast_create(p->ctx, AST_PARAM_DECL, void_tok->location);
                param->data.param_decl.name = param_name;
                param->data.param_decl.param_type = param_type;
                
                if (num_params >= cap_params) {
                    cap_params = cap_params ? cap_params * 2 : 4;
                    params = mcc_realloc(p->ctx, params,
                                         num_params * sizeof(mcc_ast_node_t*),
                                         cap_params * sizeof(mcc_ast_node_t*));
                }
                params[num_params++] = param;
                
                if (!parse_match(p, TOK_COMMA)) {
                    goto end_params;
                }
            }
        }
        
        do {
            /* Check for ellipsis */
            if (parse_match(p, TOK_ELLIPSIS)) {
                is_variadic = true;
                break;
            }
            
            mcc_type_t *param_type = parse_type_specifier(p);
            const char *param_name = NULL;
            
            if (parse_check(p, TOK_IDENT)) {
                param_name = mcc_strdup(p->ctx, p->peek->text);
                parse_advance(p);
            }
            
            /* Parse array brackets (decay to pointer) */
            while (parse_match(p, TOK_LBRACKET)) {
                /* C99: VLA in parameter */
                if (parse_check(p, TOK_STAR)) {
                    if (!parse_has_vla(p)) {
                        mcc_warning_at(p->ctx, p->peek->location,
                            "variable length arrays are a C99 extension");
                    }
                    parse_advance(p);
                } else if (!parse_check(p, TOK_RBRACKET)) {
                    parse_expression(p);
                }
                parse_expect(p, TOK_RBRACKET, "]");
                
                /* Array decays to pointer in parameter */
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
            
        } while (parse_match(p, TOK_COMMA) && !parse_check(p, TOK_ELLIPSIS));
        
        /* Check for trailing ellipsis */
        if (parse_match(p, TOK_ELLIPSIS)) {
            is_variadic = true;
        }
    }
    
end_params:
    parse_expect(p, TOK_RPAREN, ")");
    
    /* Check for function body or declaration */
    mcc_ast_node_t *body = NULL;
    bool is_definition = false;
    
    if (parse_check(p, TOK_LBRACE)) {
        body = parse_compound_stmt(p);
        is_definition = true;
    } else {
        parse_expect(p, TOK_SEMICOLON, ";");
    }
    
    mcc_ast_node_t *func = mcc_ast_create(p->ctx, AST_FUNC_DECL, loc);
    func->data.func_decl.name = name;
    func->data.func_decl.func_type = base_type;
    func->data.func_decl.params = params;
    func->data.func_decl.num_params = num_params;
    func->data.func_decl.body = body;
    func->data.func_decl.is_definition = is_definition;
    func->data.func_decl.is_static = (storage == STORAGE_STATIC);
    func->data.func_decl.is_variadic = is_variadic;
    func->data.func_decl.is_inline = base_type->is_inline;
    func->data.func_decl.is_noreturn = base_type->is_noreturn;
    return func;
}

/* ============================================================
 * Variable Declaration
 * ============================================================ */

mcc_ast_node_t *parse_variable_decl(mcc_parser_t *p, mcc_type_t *base_type,
                                     const char *name, mcc_storage_class_t storage,
                                     bool is_typedef, mcc_location_t loc)
{
    /* Build type for first declarator (may have pointer from type_specifier) */
    mcc_type_t *decl_type = base_type;
    
    /* Parse array brackets for first declarator */
    while (parse_match(p, TOK_LBRACKET)) {
        size_t array_size = 0;
        bool is_vla = false;
        
        if (!parse_check(p, TOK_RBRACKET)) {
            /* C99: VLA with * */
            if (parse_check(p, TOK_STAR)) {
                if (!parse_has_vla(p)) {
                    mcc_warning_at(p->ctx, p->peek->location,
                        "variable length arrays are a C99 extension");
                }
                parse_advance(p);
                is_vla = true;
            } else {
                mcc_ast_node_t *size_expr = parse_expression(p);
                if (size_expr && size_expr->kind == AST_INT_LIT) {
                    array_size = size_expr->data.int_lit.value;
                } else {
                    /* C99: VLA */
                    if (!parse_has_vla(p)) {
                        mcc_warning_at(p->ctx, p->peek->location,
                            "variable length arrays are a C99 extension");
                    }
                    is_vla = true;
                }
            }
        }
        parse_expect(p, TOK_RBRACKET, "]");
        
        mcc_type_t *arr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
        arr->kind = TYPE_ARRAY;
        arr->data.array.element = decl_type;
        arr->data.array.length = array_size;
        arr->data.array.is_vla = is_vla;
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
        while (parse_match(p, TOK_COMMA)) {
            /* Parse pointer(s) for this declarator */
            mcc_type_t *next_type = base_type;
            while (parse_match(p, TOK_STAR)) {
                mcc_type_t *ptr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                ptr->kind = TYPE_POINTER;
                ptr->data.pointer.pointee = next_type;
                next_type = ptr;
                
                /* Pointer qualifiers */
                while (1) {
                    if (parse_match(p, TOK_CONST)) {
                        next_type->qualifiers |= QUAL_CONST;
                    } else if (parse_match(p, TOK_VOLATILE)) {
                        next_type->qualifiers |= QUAL_VOLATILE;
                    } else if (parse_has_restrict(p) && parse_match(p, TOK_RESTRICT)) {
                        next_type->qualifiers |= QUAL_RESTRICT;
                    } else {
                        break;
                    }
                }
            }
            
            /* Get next name */
            mcc_token_t *next_tok = parse_expect(p, TOK_IDENT, "typedef name");
            const char *next_name = mcc_strdup(p->ctx, next_tok->text);
            
            /* Parse array brackets */
            while (parse_match(p, TOK_LBRACKET)) {
                size_t arr_size = 0;
                if (!parse_check(p, TOK_RBRACKET)) {
                    mcc_ast_node_t *size_expr = parse_expression(p);
                    if (size_expr && size_expr->kind == AST_INT_LIT) {
                        arr_size = size_expr->data.int_lit.value;
                    }
                }
                parse_expect(p, TOK_RBRACKET, "]");
                
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
        
        parse_expect(p, TOK_SEMICOLON, ";");
        
        /* Return a typedef declaration node for the first name */
        mcc_ast_node_t *td = mcc_ast_create(p->ctx, AST_TYPEDEF_DECL, loc);
        td->data.typedef_decl.name = name;
        td->data.typedef_decl.type = decl_type;
        return td;
    }
    
    /* Parse initializer for variable */
    mcc_ast_node_t *init = NULL;
    if (parse_match(p, TOK_ASSIGN)) {
        init = parse_initializer(p);
    }
    
    parse_expect(p, TOK_SEMICOLON, ";");
    
    mcc_ast_node_t *var = mcc_ast_create(p->ctx, AST_VAR_DECL, loc);
    var->data.var_decl.name = name;
    var->data.var_decl.var_type = decl_type;
    var->data.var_decl.init = init;
    var->data.var_decl.is_static = (storage == STORAGE_STATIC);
    var->data.var_decl.is_extern = (storage == STORAGE_EXTERN);
    return var;
}

/* ============================================================
 * Declaration (main entry point)
 * ============================================================ */

mcc_ast_node_t *parse_declaration(mcc_parser_t *p)
{
    mcc_location_t loc = p->peek->location;
    
    /* C11: _Static_assert */
    if (parse_check(p, TOK__STATIC_ASSERT) || parse_check(p, TOK_STATIC_ASSERT)) {
        if (!parse_has_static_assert(p)) {
            mcc_warning_at(p->ctx, loc,
                "'_Static_assert' is a C11 extension");
        }
        parse_advance(p);
        parse_expect(p, TOK_LPAREN, "(");
        mcc_ast_node_t *expr = parse_constant_expr(p);
        parse_expect(p, TOK_COMMA, ",");
        mcc_token_t *msg = parse_expect(p, TOK_STRING_LIT, "string literal");
        parse_expect(p, TOK_RPAREN, ")");
        parse_expect(p, TOK_SEMICOLON, ";");
        
        mcc_ast_node_t *node = mcc_ast_create(p->ctx, AST_STATIC_ASSERT, loc);
        node->data.static_assert.expr = expr;
        node->data.static_assert.message = mcc_strdup(p->ctx, msg->literal.string_val.value);
        return node;
    }
    
    /* Parse storage class */
    mcc_storage_class_t storage = STORAGE_NONE;
    bool is_typedef = false;
    
    while (1) {
        switch (p->peek->type) {
            case TOK_TYPEDEF:
                parse_advance(p);
                is_typedef = true;
                storage = STORAGE_TYPEDEF;
                continue;
            case TOK_EXTERN:
                parse_advance(p);
                storage = STORAGE_EXTERN;
                continue;
            case TOK_STATIC:
                parse_advance(p);
                storage = STORAGE_STATIC;
                continue;
            case TOK_AUTO:
                parse_advance(p);
                storage = STORAGE_AUTO;
                continue;
            case TOK_REGISTER:
                parse_advance(p);
                storage = STORAGE_REGISTER;
                continue;
            case TOK__THREAD_LOCAL:
            case TOK_THREAD_LOCAL:
                if (!parse_has_thread_local(p)) {
                    mcc_warning_at(p->ctx, p->peek->location,
                        "'_Thread_local' is a C11 extension");
                }
                parse_advance(p);
                /* TODO: Handle thread local storage */
                continue;
            default:
                break;
        }
        break;
    }
    
    /* Parse type specifier */
    mcc_type_t *base_type = parse_type_specifier(p);
    
    /* Parse declarator(s) */
    if (parse_check(p, TOK_SEMICOLON)) {
        /* Type declaration only (e.g., struct definition) */
        parse_advance(p);
        return NULL; /* TODO: Return struct/union/enum declaration */
    }
    
    /* Get declarator name */
    mcc_token_t *name_tok = parse_expect(p, TOK_IDENT, "identifier");
    const char *name = mcc_strdup(p->ctx, name_tok->text);
    
    /* Check for function declaration */
    if (parse_check(p, TOK_LPAREN)) {
        return parse_function_decl(p, base_type, name, storage, loc);
    }
    
    /* Variable/typedef declaration */
    return parse_variable_decl(p, base_type, name, storage, is_typedef, loc);
}
