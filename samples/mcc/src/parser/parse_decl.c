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

/* Forward declaration for parse_variable_decl with attributes */
static mcc_ast_node_t *parse_variable_decl_with_attrs(mcc_parser_t *p, mcc_type_t *decl_type,
                                     const char *name, mcc_storage_class_t storage,
                                     bool is_typedef, mcc_location_t loc,
                                     mcc_attribute_t *attrs);

mcc_ast_node_t *parse_variable_decl(mcc_parser_t *p, mcc_type_t *decl_type,
                                     const char *name, mcc_storage_class_t storage,
                                     bool is_typedef, mcc_location_t loc)
{
    return parse_variable_decl_with_attrs(p, decl_type, name, storage, is_typedef, loc, NULL);
}

static mcc_ast_node_t *parse_variable_decl_with_attrs(mcc_parser_t *p, mcc_type_t *decl_type,
                                     const char *name, mcc_storage_class_t storage,
                                     bool is_typedef, mcc_location_t loc,
                                     mcc_attribute_t *attrs)
{
    /* Type is already fully parsed by parse_declarator */
    
    /* Handle typedef - register the name */
    if (is_typedef) {
        /* Register first typedef name */
        mcc_typedef_entry_t *entry = mcc_alloc(p->ctx, sizeof(mcc_typedef_entry_t));
        entry->name = name;
        entry->type = decl_type;
        entry->next = p->typedefs;
        p->typedefs = entry;
        
        /* Handle multiple typedef names: typedef int A, *B, **C; */
        while (parse_match(p, TOK_COMMA)) {
            /* Parse the next declarator - need to get base type from original */
            mcc_type_t *base = decl_type;
            /* Strip pointers/arrays to get base type for next declarator */
            while (base && base->kind == TYPE_POINTER) {
                base = base->data.pointer.pointee;
            }
            while (base && base->kind == TYPE_ARRAY) {
                base = base->data.array.element;
            }
            
            parse_declarator_result_t next_decl = parse_declarator(p, base, false);
            if (next_decl.name) {
                mcc_typedef_entry_t *next_entry = mcc_alloc(p->ctx, sizeof(mcc_typedef_entry_t));
                next_entry->name = next_decl.name;
                next_entry->type = next_decl.type;
                next_entry->next = p->typedefs;
                p->typedefs = next_entry;
            }
        }
        
        parse_expect(p, TOK_SEMICOLON, ";");
        
        /* Return a typedef declaration node */
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
    
    /* Create first variable declaration */
    mcc_ast_node_t *var = mcc_ast_create(p->ctx, AST_VAR_DECL, loc);
    var->data.var_decl.name = name;
    var->data.var_decl.var_type = decl_type;
    var->data.var_decl.init = init;
    var->data.var_decl.is_static = (storage == STORAGE_STATIC);
    var->data.var_decl.is_extern = (storage == STORAGE_EXTERN);
    var->data.var_decl.attrs = attrs;
    
    /* Check for multiple variable declarations: int a, b, c; */
    if (!parse_check(p, TOK_COMMA)) {
        parse_expect(p, TOK_SEMICOLON, ";");
        return var;
    }
    
    /* Multiple declarations - create a declaration list */
    mcc_ast_node_t **decls = NULL;
    size_t num_decls = 0;
    size_t cap_decls = 0;
    
    /* Add first declaration to list */
    if (num_decls >= cap_decls) {
        cap_decls = cap_decls ? cap_decls * 2 : 4;
        decls = mcc_realloc(p->ctx, decls, num_decls * sizeof(mcc_ast_node_t*),
                            cap_decls * sizeof(mcc_ast_node_t*));
    }
    decls[num_decls++] = var;
    
    /* Get base type by stripping pointers/arrays from decl_type */
    mcc_type_t *base = decl_type;
    while (base && base->kind == TYPE_POINTER) {
        base = base->data.pointer.pointee;
    }
    while (base && base->kind == TYPE_ARRAY) {
        base = base->data.array.element;
    }
    
    /* Parse additional declarations */
    while (parse_match(p, TOK_COMMA)) {
        /* Parse next declarator */
        parse_declarator_result_t next_decl = parse_declarator(p, base, false);
        
        /* Parse initializer for this variable */
        mcc_ast_node_t *next_init = NULL;
        if (parse_match(p, TOK_ASSIGN)) {
            next_init = parse_initializer(p);
        }
        
        /* Create variable declaration node */
        mcc_ast_node_t *next_var = mcc_ast_create(p->ctx, AST_VAR_DECL, loc);
        next_var->data.var_decl.name = next_decl.name;
        next_var->data.var_decl.var_type = next_decl.type;
        next_var->data.var_decl.init = next_init;
        next_var->data.var_decl.is_static = (storage == STORAGE_STATIC);
        next_var->data.var_decl.is_extern = (storage == STORAGE_EXTERN);
        
        /* Add to list */
        if (num_decls >= cap_decls) {
            cap_decls = cap_decls ? cap_decls * 2 : 4;
            decls = mcc_realloc(p->ctx, decls, num_decls * sizeof(mcc_ast_node_t*),
                                cap_decls * sizeof(mcc_ast_node_t*));
        }
        decls[num_decls++] = next_var;
    }
    
    parse_expect(p, TOK_SEMICOLON, ";");
    
    /* Create declaration list node */
    mcc_ast_node_t *list = mcc_ast_create(p->ctx, AST_DECL_LIST, loc);
    list->data.decl_list.decls = decls;
    list->data.decl_list.num_decls = num_decls;
    
    return list;
}

/* ============================================================
 * Declaration (main entry point)
 * ============================================================ */

/* Parse a single C23 attribute and return its kind */
static mcc_attr_kind_t parse_attribute_name(const char *name)
{
    if (strcmp(name, "deprecated") == 0) return MCC_ATTR_DEPRECATED;
    if (strcmp(name, "fallthrough") == 0) return MCC_ATTR_FALLTHROUGH;
    if (strcmp(name, "nodiscard") == 0) return MCC_ATTR_NODISCARD;
    if (strcmp(name, "maybe_unused") == 0) return MCC_ATTR_MAYBE_UNUSED;
    if (strcmp(name, "noreturn") == 0) return MCC_ATTR_NORETURN;
    if (strcmp(name, "unsequenced") == 0) return MCC_ATTR_UNSEQUENCED;
    if (strcmp(name, "reproducible") == 0) return MCC_ATTR_REPRODUCIBLE;
    return MCC_ATTR_UNKNOWN;
}

/* Parse C23 attributes [[...]] and return attribute list */
static mcc_attribute_t *parse_attributes(mcc_parser_t *p)
{
    mcc_attribute_t *attrs = NULL;
    mcc_attribute_t *tail = NULL;
    
    while (parse_check(p, TOK_LBRACKET2)) {
        if (!parse_has_feature(p, MCC_FEAT_ATTR_SYNTAX)) {
            mcc_warning_at(p->ctx, p->peek->location,
                "attribute syntax [[...]] is a C23 feature");
        }
        parse_advance(p);  /* Skip [[ */
        
        /* Parse attributes inside [[ ]] */
        while (!parse_check(p, TOK_RBRACKET2) && !parse_check(p, TOK_EOF)) {
            if (parse_check(p, TOK_IDENT)) {
                mcc_attribute_t *attr = mcc_alloc(p->ctx, sizeof(mcc_attribute_t));
                attr->name = mcc_strdup(p->ctx, p->peek->text);
                attr->kind = parse_attribute_name(p->peek->text);
                attr->message = NULL;
                attr->alignment = 0;
                attr->next = NULL;
                
                parse_advance(p);  /* Consume attribute name */
                
                /* Check for attribute arguments */
                if (parse_match(p, TOK_LPAREN)) {
                    /* Parse attribute argument (e.g., deprecated("message")) */
                    if (parse_check(p, TOK_STRING_LIT)) {
                        attr->message = mcc_strdup(p->ctx, p->peek->literal.string_val.value);
                        parse_advance(p);
                    } else if (parse_check(p, TOK_INT_LIT)) {
                        attr->alignment = (int)p->peek->literal.int_val.value;
                        parse_advance(p);
                    }
                    parse_expect(p, TOK_RPAREN, ")");
                }
                
                /* Add to list */
                if (!attrs) attrs = attr;
                if (tail) tail->next = attr;
                tail = attr;
            }
            
            /* Skip comma between attributes */
            if (!parse_match(p, TOK_COMMA)) {
                break;
            }
        }
        
        parse_expect(p, TOK_RBRACKET2, "]]");
    }
    
    return attrs;
}

/* Skip C23 attributes [[...]] (for places where we don't need to store them) */
static void parse_skip_attributes(mcc_parser_t *p)
{
    mcc_attribute_t *attrs = parse_attributes(p);
    (void)attrs;  /* Attributes parsed but not used here */
}

mcc_ast_node_t *parse_declaration(mcc_parser_t *p)
{
    mcc_location_t loc = p->peek->location;
    
    /* C23: Parse attributes [[...]] */
    mcc_attribute_t *attrs = parse_attributes(p);
    
    /* C11: _Static_assert */
    if (parse_check(p, TOK__STATIC_ASSERT) || parse_check(p, TOK_STATIC_ASSERT)) {
        if (!parse_has_static_assert(p)) {
            mcc_error_at(p->ctx, loc,
                "'_Static_assert' requires C11 or later");
            /* Skip to semicolon to recover */
            while (!parse_check(p, TOK_SEMICOLON) && !parse_check(p, TOK_EOF)) {
                parse_advance(p);
            }
            if (parse_check(p, TOK_SEMICOLON)) parse_advance(p);
            return NULL;
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
    
    /* C11: _Alignas - parse alignment specifier */
    int alignment = 0;
    while (parse_check(p, TOK__ALIGNAS) || parse_check(p, TOK_ALIGNAS)) {
        if (!parse_has_alignas(p)) {
            mcc_error_at(p->ctx, loc,
                "'_Alignas' requires C11 or later");
            /* Skip to semicolon to recover */
            while (!parse_check(p, TOK_SEMICOLON) && !parse_check(p, TOK_EOF)) {
                parse_advance(p);
            }
            if (parse_check(p, TOK_SEMICOLON)) parse_advance(p);
            return NULL;
        }
        parse_advance(p);
        parse_expect(p, TOK_LPAREN, "(");
        
        /* _Alignas can take a type or a constant expression */
        if (parse_is_type_start(p)) {
            mcc_type_t *type = parse_type_specifier(p);
            if (type) {
                alignment = (int)mcc_type_alignof(type);
            }
        } else {
            mcc_ast_node_t *expr = parse_constant_expr(p);
            if (expr && expr->kind == AST_INT_LIT) {
                alignment = (int)expr->data.int_lit.value;
            }
        }
        parse_expect(p, TOK_RPAREN, ")");
    }
    
    /* C11: _Noreturn - give clear error if not available */
    if (parse_check(p, TOK__NORETURN)) {
        if (!parse_has_noreturn(p)) {
            mcc_error_at(p->ctx, loc,
                "'_Noreturn' requires C11 or later");
            /* Skip to semicolon to recover */
            while (!parse_check(p, TOK_SEMICOLON) && !parse_check(p, TOK_EOF)) {
                parse_advance(p);
            }
            if (parse_check(p, TOK_SEMICOLON)) parse_advance(p);
            return NULL;
        }
    }
    
    /* C11: _Thread_local - give clear error if not available */
    if (parse_check(p, TOK__THREAD_LOCAL)) {
        if (!parse_has_thread_local(p)) {
            mcc_error_at(p->ctx, loc,
                "'_Thread_local' requires C11 or later");
            /* Skip to semicolon to recover */
            while (!parse_check(p, TOK_SEMICOLON) && !parse_check(p, TOK_EOF)) {
                parse_advance(p);
            }
            if (parse_check(p, TOK_SEMICOLON)) parse_advance(p);
            return NULL;
        }
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
                /* Note: C11 check already done above */
                parse_advance(p);
                /* Thread local can combine with static or extern */
                if (storage == STORAGE_NONE) {
                    storage = STORAGE_THREAD_LOCAL;
                } else if (storage == STORAGE_STATIC || storage == STORAGE_EXTERN) {
                    /* Valid combination - keep the existing storage class */
                    /* The thread_local aspect will be tracked separately */
                } else {
                    mcc_error_at(p->ctx, p->peek->location,
                        "_Thread_local can only combine with static or extern");
                }
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
        
        /* Return appropriate declaration node for struct/union/enum */
        if (base_type->kind == TYPE_STRUCT) {
            mcc_ast_node_t *decl = mcc_ast_create(p->ctx, AST_STRUCT_DECL, loc);
            decl->data.struct_decl.tag = base_type->data.record.tag;
            decl->data.struct_decl.fields = NULL; /* Fields stored in type */
            decl->data.struct_decl.num_fields = 0;
            decl->data.struct_decl.is_definition = base_type->data.record.is_complete;
            return decl;
        } else if (base_type->kind == TYPE_UNION) {
            mcc_ast_node_t *decl = mcc_ast_create(p->ctx, AST_UNION_DECL, loc);
            decl->data.struct_decl.tag = base_type->data.record.tag;
            decl->data.struct_decl.fields = NULL;
            decl->data.struct_decl.num_fields = 0;
            decl->data.struct_decl.is_definition = base_type->data.record.is_complete;
            return decl;
        } else if (base_type->kind == TYPE_ENUM) {
            mcc_ast_node_t *decl = mcc_ast_create(p->ctx, AST_ENUM_DECL, loc);
            decl->data.enum_decl.tag = base_type->data.enumeration.tag;
            decl->data.enum_decl.enumerators = NULL;
            decl->data.enum_decl.num_enumerators = base_type->data.enumeration.num_constants;
            decl->data.enum_decl.is_definition = base_type->data.enumeration.is_complete;
            return decl;
        }
        
        /* Other type declarations (e.g., forward declarations) */
        return NULL;
    }
    
    /* Parse declarator (handles complex types like int (*arr)[10]) */
    parse_declarator_result_t decl_result = parse_declarator(p, base_type, false);
    const char *name = decl_result.name;
    mcc_type_t *decl_type = decl_result.type;
    
    /* Check if this is a function type (function declaration/definition) */
    if (decl_type->kind == TYPE_FUNCTION) {
        /* Function declaration - body may follow */
        mcc_ast_node_t *body = NULL;
        bool is_definition = false;
        
        if (parse_check(p, TOK_LBRACE)) {
            body = parse_compound_stmt(p);
            is_definition = true;
        } else {
            parse_expect(p, TOK_SEMICOLON, ";");
        }
        
        /* Convert mcc_func_param_t linked list to AST node array */
        mcc_ast_node_t **params = NULL;
        size_t num_params = decl_type->data.function.num_params;
        if (num_params > 0 && decl_type->data.function.params) {
            params = mcc_alloc(p->ctx, num_params * sizeof(mcc_ast_node_t*));
            mcc_func_param_t *param = decl_type->data.function.params;
            for (size_t i = 0; i < num_params && param; i++, param = param->next) {
                mcc_ast_node_t *param_node = mcc_ast_create(p->ctx, AST_PARAM_DECL, loc);
                param_node->data.param_decl.name = param->name;
                param_node->data.param_decl.param_type = param->type;
                params[i] = param_node;
            }
        }
        
        mcc_ast_node_t *func = mcc_ast_create(p->ctx, AST_FUNC_DECL, loc);
        func->data.func_decl.name = name;
        func->data.func_decl.func_type = decl_type->data.function.return_type;
        func->data.func_decl.params = params;
        func->data.func_decl.num_params = num_params;
        func->data.func_decl.body = body;
        func->data.func_decl.is_definition = is_definition;
        func->data.func_decl.is_static = (storage == STORAGE_STATIC);
        func->data.func_decl.is_variadic = decl_type->data.function.is_variadic;
        func->data.func_decl.is_inline = base_type->is_inline;
        func->data.func_decl.is_noreturn = base_type->is_noreturn;
        func->data.func_decl.attrs = attrs;
        
        /* Check for [[noreturn]] attribute */
        for (mcc_attribute_t *a = attrs; a; a = a->next) {
            if (a->kind == MCC_ATTR_NORETURN) {
                func->data.func_decl.is_noreturn = true;
            }
        }
        return func;
    }
    
    /* Variable/typedef declaration */
    return parse_variable_decl_with_attrs(p, decl_type, name, storage, is_typedef, loc, attrs);
}
