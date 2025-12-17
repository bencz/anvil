/*
 * MCC - Micro C Compiler
 * Type Parser
 * 
 * This file handles parsing of C type specifiers:
 * - Basic types (void, char, short, int, long, float, double)
 * - Type qualifiers (const, volatile, restrict)
 * - Storage classes (auto, register, static, extern, typedef)
 * - Derived types (pointers, arrays)
 * - Aggregate types (struct, union, enum)
 * - Typedef names
 */

#include "parse_internal.h"

/* ============================================================
 * Type Start Detection
 * ============================================================ */

bool parse_is_type_start(mcc_parser_t *p)
{
    switch (p->peek->type) {
        /* Basic types */
        case TOK_VOID:
        case TOK_CHAR:
        case TOK_SHORT:
        case TOK_INT:
        case TOK_LONG:
        case TOK_FLOAT:
        case TOK_DOUBLE:
        case TOK_SIGNED:
        case TOK_UNSIGNED:
        /* Aggregate types */
        case TOK_STRUCT:
        case TOK_UNION:
        case TOK_ENUM:
        /* Type qualifiers */
        case TOK_CONST:
        case TOK_VOLATILE:
            return true;
        
        /* C99: restrict */
        case TOK_RESTRICT:
            return parse_has_restrict(p);
        
        /* C99: _Bool */
        case TOK__BOOL:
            return parse_has_bool(p);
        
        /* C99: _Complex, _Imaginary */
        case TOK__COMPLEX:
        case TOK__IMAGINARY:
            return parse_has_feature(p, MCC_FEAT_COMPLEX);
        
        /* C11: _Atomic */
        case TOK__ATOMIC:
            return parse_has_atomic(p);
        
        /* C23: bool keyword */
        case TOK_BOOL:
            return parse_has_bool_keyword(p);
        
        /* C23: typeof, typeof_unqual */
        case TOK_TYPEOF:
        case TOK_TYPEOF_UNQUAL:
            return parse_has_feature(p, MCC_FEAT_TYPEOF);
        
        /* Typedef name */
        case TOK_IDENT:
            if (p->symtab) {
                return mcc_symtab_is_typedef(p->symtab, p->peek->text);
            }
            return parse_is_typedef_name(p, p->peek->text);
            
        default:
            return false;
    }
}

/* ============================================================
 * Declaration Start Detection
 * ============================================================ */

bool parse_is_declaration_start(mcc_parser_t *p)
{
    switch (p->peek->type) {
        /* Storage class specifiers */
        case TOK_TYPEDEF:
        case TOK_EXTERN:
        case TOK_STATIC:
        case TOK_AUTO:
        case TOK_REGISTER:
            return true;
        
        /* C99: inline */
        case TOK_INLINE:
            return parse_has_inline(p);
        
        /* C11: _Noreturn, _Thread_local, _Alignas, _Static_assert
         * Always return true so parser can give clear error messages */
        case TOK__NORETURN:
        case TOK__THREAD_LOCAL:
        case TOK__ALIGNAS:
        case TOK__STATIC_ASSERT:
            return true;
        
        /* C23: constexpr */
        case TOK_CONSTEXPR:
            return parse_has_constexpr(p);
        
        /* C23: thread_local, alignas, static_assert (without underscore) */
        case TOK_THREAD_LOCAL:
            return parse_has_thread_local(p);
        case TOK_ALIGNAS:
            return parse_has_alignas(p);
        case TOK_STATIC_ASSERT:
            return parse_has_static_assert(p);
            
        default:
            return parse_is_type_start(p);
    }
}

/* ============================================================
 * Typedef Name Check
 * ============================================================ */

bool parse_is_typedef_name(mcc_parser_t *p, const char *name)
{
    for (mcc_typedef_entry_t *t = p->typedefs; t; t = t->next) {
        if (strcmp(t->name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* ============================================================
 * Struct/Union Parsing
 * ============================================================ */

mcc_type_t *parse_struct_or_union(mcc_parser_t *p, bool is_union)
{
    mcc_location_t loc = p->peek->location;
    parse_advance(p); /* consume 'struct' or 'union' */
    
    /* Get tag name if present */
    const char *tag = NULL;
    if (parse_check(p, TOK_IDENT)) {
        tag = mcc_strdup(p->ctx, p->peek->text);
        parse_advance(p);
    }
    
    mcc_type_t *stype = NULL;
    
    /* Check if this is a definition or reference */
    if (parse_match(p, TOK_LBRACE)) {
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
        
        while (!parse_check(p, TOK_RBRACE) && !parse_check(p, TOK_EOF)) {
            /* C11: _Static_assert in struct */
            if (parse_check(p, TOK__STATIC_ASSERT) || parse_check(p, TOK_STATIC_ASSERT)) {
                if (!parse_has_static_assert(p)) {
                    mcc_warning_at(p->ctx, p->peek->location,
                        "'_Static_assert' is a C11 extension");
                }
                parse_advance(p);
                parse_expect(p, TOK_LPAREN, "(");
                mcc_ast_node_t *expr = parse_constant_expr(p);
                parse_expect(p, TOK_COMMA, ",");
                mcc_token_t *msg = parse_expect(p, TOK_STRING_LIT, "string literal");
                parse_expect(p, TOK_RPAREN, ")");
                parse_expect(p, TOK_SEMICOLON, ";");
                
                /* Evaluate static assert at compile time */
                if (expr && expr->kind == AST_INT_LIT) {
                    if (expr->data.int_lit.value == 0) {
                        mcc_error_at(p->ctx, expr->location,
                            "static assertion failed: %s", msg->literal.string_val.value);
                    }
                }
                continue;
            }
            
            mcc_type_t *field_base_type = parse_type_specifier(p);
            
            /* C11: Anonymous struct/union member */
            if ((field_base_type->kind == TYPE_STRUCT || field_base_type->kind == TYPE_UNION) &&
                parse_check(p, TOK_SEMICOLON)) {
                if (!parse_has_anonymous_struct(p)) {
                    mcc_warning_at(p->ctx, loc,
                        "anonymous struct/union members are a C11 extension");
                }
                parse_advance(p); /* consume ';' */
                
                /* Add anonymous member */
                mcc_struct_field_t *field = mcc_alloc(p->ctx, sizeof(mcc_struct_field_t));
                field->name = NULL; /* Anonymous */
                field->type = field_base_type;
                field->next = NULL;
                
                if (!fields) fields = field;
                if (field_tail) field_tail->next = field;
                field_tail = field;
                num_fields++;
                continue;
            }
            
            /* Parse field declarator(s) */
            do {
                /* Parse pointer(s) for this field */
                mcc_type_t *field_type = field_base_type;
                while (parse_match(p, TOK_STAR)) {
                    mcc_type_t *ptr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                    ptr->kind = TYPE_POINTER;
                    ptr->data.pointer.pointee = field_type;
                    field_type = ptr;
                    
                    /* Pointer qualifiers */
                    while (parse_match(p, TOK_CONST)) {
                        field_type->qualifiers |= QUAL_CONST;
                    }
                    while (parse_match(p, TOK_VOLATILE)) {
                        field_type->qualifiers |= QUAL_VOLATILE;
                    }
                    if (parse_has_restrict(p) && parse_match(p, TOK_RESTRICT)) {
                        field_type->qualifiers |= QUAL_RESTRICT;
                    }
                }
                
                mcc_token_t *field_name = parse_expect(p, TOK_IDENT, "field name");
                
                /* Parse array brackets for this field */
                while (parse_match(p, TOK_LBRACKET)) {
                    size_t arr_size = 0;
                    bool is_flexible = false;
                    
                    if (!parse_check(p, TOK_RBRACKET)) {
                        mcc_ast_node_t *size_expr = parse_expression(p);
                        if (size_expr && size_expr->kind == AST_INT_LIT) {
                            arr_size = size_expr->data.int_lit.value;
                        }
                    } else {
                        /* C99: Flexible array member */
                        is_flexible = true;
                        if (!parse_has_flexible_array(p)) {
                            mcc_warning_at(p->ctx, field_name->location,
                                "flexible array members are a C99 extension");
                        }
                    }
                    parse_expect(p, TOK_RBRACKET, "]");
                    
                    mcc_type_t *arr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                    arr->kind = TYPE_ARRAY;
                    arr->data.array.element = field_type;
                    arr->data.array.length = arr_size;
                    arr->data.array.is_flexible = is_flexible;
                    field_type = arr;
                }
                
                /* Parse bitfield width */
                int bitfield_width = 0;
                if (parse_match(p, TOK_COLON)) {
                    /* Bitfield */
                    mcc_ast_node_t *width_expr = parse_constant_expr(p);
                    if (width_expr && width_expr->kind == AST_INT_LIT) {
                        bitfield_width = (int)width_expr->data.int_lit.value;
                        if (bitfield_width < 0) {
                            mcc_error_at(p->ctx, field_name->location,
                                "bitfield width cannot be negative");
                            bitfield_width = 0;
                        } else if (bitfield_width > 64) {
                            mcc_error_at(p->ctx, field_name->location,
                                "bitfield width exceeds maximum (64 bits)");
                            bitfield_width = 64;
                        }
                    } else {
                        mcc_error_at(p->ctx, field_name->location,
                            "bitfield width must be a constant expression");
                    }
                }
                
                mcc_struct_field_t *field = mcc_alloc(p->ctx, sizeof(mcc_struct_field_t));
                field->name = mcc_strdup(p->ctx, field_name->text);
                field->type = field_type;
                field->bitfield_width = bitfield_width;
                field->next = NULL;
                
                if (!fields) fields = field;
                if (field_tail) field_tail->next = field;
                field_tail = field;
                num_fields++;
                
            } while (parse_match(p, TOK_COMMA));
            
            parse_expect(p, TOK_SEMICOLON, ";");
        }
        
        parse_expect(p, TOK_RBRACE, "}");
        
        stype->data.record.fields = fields;
        stype->data.record.num_fields = num_fields;
        stype->data.record.is_complete = true;
        
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
        mcc_error_at(p->ctx, loc, "anonymous struct/union must have a definition");
        stype = mcc_alloc(p->ctx, sizeof(mcc_type_t));
        stype->kind = TYPE_INT;
    }
    
    return stype;
}

/* ============================================================
 * Enum Parsing
 * ============================================================ */

mcc_type_t *parse_enum(mcc_parser_t *p)
{
    mcc_location_t loc = p->peek->location;
    parse_advance(p); /* consume 'enum' */
    
    /* Get tag name if present */
    const char *tag = NULL;
    if (parse_check(p, TOK_IDENT)) {
        tag = mcc_strdup(p->ctx, p->peek->text);
        parse_advance(p);
    }
    
    mcc_type_t *etype = mcc_alloc(p->ctx, sizeof(mcc_type_t));
    etype->kind = TYPE_ENUM;
    etype->data.enumeration.tag = tag;
    etype->data.enumeration.constants = NULL;
    etype->data.enumeration.num_constants = 0;
    etype->data.enumeration.is_complete = false;
    
    if (parse_match(p, TOK_LBRACE)) {
        /* Parse enumerators */
        int64_t next_value = 0;
        mcc_enum_const_t *constants = NULL;
        mcc_enum_const_t *const_tail = NULL;
        int num_constants = 0;
        
        while (!parse_check(p, TOK_RBRACE) && !parse_check(p, TOK_EOF)) {
            mcc_token_t *name_tok = parse_expect(p, TOK_IDENT, "enumerator name");
            
            int64_t value = next_value;
            if (parse_match(p, TOK_ASSIGN)) {
                mcc_ast_node_t *val_expr = parse_constant_expr(p);
                if (val_expr && val_expr->kind == AST_INT_LIT) {
                    value = val_expr->data.int_lit.value;
                }
            }
            
            /* Create enum constant */
            mcc_enum_const_t *econst = mcc_alloc(p->ctx, sizeof(mcc_enum_const_t));
            econst->name = mcc_strdup(p->ctx, name_tok->text);
            econst->value = value;
            econst->next = NULL;
            
            if (!constants) constants = econst;
            if (const_tail) const_tail->next = econst;
            const_tail = econst;
            num_constants++;
            
            next_value = value + 1;
            
            if (!parse_match(p, TOK_COMMA)) {
                break;
            }
            
            /* C99: Allow trailing comma */
            if (parse_check(p, TOK_RBRACE)) {
                break;
            }
        }
        
        parse_expect(p, TOK_RBRACE, "}");
        
        etype->data.enumeration.constants = constants;
        etype->data.enumeration.num_constants = num_constants;
        etype->data.enumeration.is_complete = true;
    } else if (!tag) {
        mcc_error_at(p->ctx, loc, "anonymous enum must have a definition");
    }
    
    return etype;
}

/* ============================================================
 * Type Specifier Parsing
 * ============================================================ */

mcc_type_t *parse_type_specifier(mcc_parser_t *p)
{
    bool is_unsigned = false;
    bool is_signed = false;
    bool is_const = false;
    bool is_volatile = false;
    bool is_restrict = false;
    bool is_inline = false;
    bool is_noreturn = false;
    int type_spec = 0; /* 0=none, 1=void, 2=char, 3=short, 4=int, 5=long, 6=float, 7=double, 8=long long */
    int long_count = 0;
    
    while (1) {
        switch (p->peek->type) {
            case TOK_CONST:
                parse_advance(p);
                is_const = true;
                continue;
            case TOK_VOLATILE:
                parse_advance(p);
                is_volatile = true;
                continue;
            case TOK_RESTRICT:
                if (!parse_has_restrict(p)) {
                    mcc_warning_at(p->ctx, p->peek->location,
                        "'restrict' is a C99 extension");
                }
                parse_advance(p);
                is_restrict = true;
                continue;
            case TOK_INLINE:
                if (!parse_has_inline(p)) {
                    mcc_warning_at(p->ctx, p->peek->location,
                        "'inline' is a C99 extension");
                }
                parse_advance(p);
                is_inline = true;
                continue;
            case TOK__NORETURN:
                if (!parse_has_noreturn(p)) {
                    mcc_warning_at(p->ctx, p->peek->location,
                        "'_Noreturn' is a C11 extension");
                }
                parse_advance(p);
                is_noreturn = true;
                continue;
            case TOK__ATOMIC:
                if (!parse_has_atomic(p)) {
                    mcc_warning_at(p->ctx, p->peek->location,
                        "'_Atomic' is a C11 extension");
                }
                parse_advance(p);
                /* _Atomic can be used as type qualifier or type specifier */
                /* For now, treat as qualifier - skip the type if in parens */
                if (parse_match(p, TOK_LPAREN)) {
                    /* _Atomic(type) form - parse and skip the type */
                    (void)parse_type_specifier(p);
                    parse_expect(p, TOK_RPAREN, ")");
                }
                continue;
            case TOK_UNSIGNED:
                parse_advance(p);
                is_unsigned = true;
                continue;
            case TOK_SIGNED:
                parse_advance(p);
                is_signed = true;
                continue;
            case TOK_VOID:
                parse_advance(p);
                type_spec = 1;
                break;
            case TOK_CHAR:
                parse_advance(p);
                type_spec = 2;
                break;
            case TOK_SHORT:
                parse_advance(p);
                type_spec = 3;
                continue; /* Allow 'short int' */
            case TOK_INT:
                parse_advance(p);
                /* 'int' after 'short' or 'long' is redundant but valid */
                if (type_spec == 0) {
                    type_spec = 4;
                }
                /* If type_spec is already set (short, long, long long), keep it */
                continue;
            case TOK_LONG:
                parse_advance(p);
                long_count++;
                if (long_count == 1) {
                    type_spec = 5; /* long */
                } else if (long_count == 2) {
                    /* C99: long long */
                    if (!parse_has_long_long(p)) {
                        mcc_warning_at(p->ctx, p->current->location,
                            "'long long' is a C99 extension");
                    }
                    type_spec = 8; /* long long */
                }
                continue; /* Allow 'long long' or 'long int' */
            case TOK_FLOAT:
                parse_advance(p);
                type_spec = 6;
                break;
            case TOK_DOUBLE:
                parse_advance(p);
                type_spec = 7;
                break;
            case TOK__BOOL:
                if (!parse_has_bool(p)) {
                    mcc_warning_at(p->ctx, p->peek->location,
                        "'_Bool' is a C99 extension");
                }
                parse_advance(p);
                type_spec = 9; /* _Bool */
                break;
            case TOK_BOOL:
                if (!parse_has_bool_keyword(p)) {
                    mcc_warning_at(p->ctx, p->peek->location,
                        "'bool' as keyword is a C23 extension");
                }
                parse_advance(p);
                type_spec = 9; /* bool */
                break;
            case TOK_TYPEOF:
            case TOK_TYPEOF_UNQUAL: {
                bool is_unqual = (p->peek->type == TOK_TYPEOF_UNQUAL);
                if (!parse_has_feature(p, MCC_FEAT_TYPEOF)) {
                    mcc_warning_at(p->ctx, p->peek->location,
                        "'typeof' is a C23 extension");
                }
                parse_advance(p);
                parse_expect(p, TOK_LPAREN, "(");
                
                mcc_type_t *result_type;
                if (parse_is_type_start(p)) {
                    result_type = parse_type_specifier(p);
                } else {
                    /* typeof(expression) - get type from expression */
                    mcc_ast_node_t *expr = parse_expression(p);
                    /* For now, return int as placeholder - proper implementation
                     * would need semantic analysis to determine expression type */
                    result_type = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                    result_type->kind = TYPE_INT;
                    result_type->size = 4;
                    result_type->align = 4;
                    (void)expr;
                }
                parse_expect(p, TOK_RPAREN, ")");
                
                /* typeof_unqual removes qualifiers */
                if (is_unqual && result_type) {
                    result_type->qualifiers = 0;
                }
                
                /* Handle additional qualifiers */
                if (is_const && result_type) result_type->qualifiers |= QUAL_CONST;
                if (is_volatile && result_type) result_type->qualifiers |= QUAL_VOLATILE;
                
                return result_type;
            }
            case TOK_STRUCT:
            case TOK_UNION: {
                bool is_union = (p->peek->type == TOK_UNION);
                mcc_type_t *stype = parse_struct_or_union(p, is_union);
                
                /* Handle qualifiers */
                if (is_const) stype->qualifiers |= QUAL_CONST;
                if (is_volatile) stype->qualifiers |= QUAL_VOLATILE;
                
                /* Parse pointer */
                while (parse_match(p, TOK_STAR)) {
                    mcc_type_t *ptr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                    ptr->kind = TYPE_POINTER;
                    ptr->data.pointer.pointee = stype;
                    stype = ptr;
                    
                    /* Pointer qualifiers */
                    while (1) {
                        if (parse_match(p, TOK_CONST)) {
                            stype->qualifiers |= QUAL_CONST;
                        } else if (parse_match(p, TOK_VOLATILE)) {
                            stype->qualifiers |= QUAL_VOLATILE;
                        } else if (parse_has_restrict(p) && parse_match(p, TOK_RESTRICT)) {
                            stype->qualifiers |= QUAL_RESTRICT;
                        } else {
                            break;
                        }
                    }
                }
                
                return stype;
            }
            case TOK_ENUM: {
                mcc_type_t *etype = parse_enum(p);
                
                /* Handle qualifiers */
                if (is_const) etype->qualifiers |= QUAL_CONST;
                if (is_volatile) etype->qualifiers |= QUAL_VOLATILE;
                
                /* Parse pointer */
                while (parse_match(p, TOK_STAR)) {
                    mcc_type_t *ptr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                    ptr->kind = TYPE_POINTER;
                    ptr->data.pointer.pointee = etype;
                    etype = ptr;
                }
                
                return etype;
            }
            default:
                /* Check for typedef name */
                if (p->peek->type == TOK_IDENT && type_spec == 0) {
                    for (mcc_typedef_entry_t *t = p->typedefs; t; t = t->next) {
                        if (strcmp(t->name, p->peek->text) == 0) {
                            parse_advance(p);
                            mcc_type_t *type = t->type;
                            
                            /* Handle qualifiers */
                            if (is_const) type->qualifiers |= QUAL_CONST;
                            if (is_volatile) type->qualifiers |= QUAL_VOLATILE;
                            
                            /* Parse pointer */
                            while (parse_match(p, TOK_STAR)) {
                                mcc_type_t *ptr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                                ptr->kind = TYPE_POINTER;
                                ptr->data.pointer.pointee = type;
                                type = ptr;
                                
                                /* Pointer qualifiers */
                                while (1) {
                                    if (parse_match(p, TOK_CONST)) {
                                        type->qualifiers |= QUAL_CONST;
                                    } else if (parse_match(p, TOK_VOLATILE)) {
                                        type->qualifiers |= QUAL_VOLATILE;
                                    } else if (parse_has_restrict(p) && parse_match(p, TOK_RESTRICT)) {
                                        type->qualifiers |= QUAL_RESTRICT;
                                    } else {
                                        break;
                                    }
                                }
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
    
    /* Create type */
    mcc_type_t *type = mcc_alloc(p->ctx, sizeof(mcc_type_t));
    
    switch (type_spec) {
        case 1: type->kind = TYPE_VOID; type->size = 0; type->align = 1; break;
        case 2: type->kind = TYPE_CHAR; type->size = 1; type->align = 1; break;
        case 3: type->kind = TYPE_SHORT; type->size = 2; type->align = 2; break;
        case 4: type->kind = TYPE_INT; type->size = 4; type->align = 4; break;
        case 5: type->kind = TYPE_LONG; type->size = 4; type->align = 4; break; /* Will be corrected by codegen_sizeof */
        case 6: type->kind = TYPE_FLOAT; type->size = 4; type->align = 4; break;
        case 7: type->kind = TYPE_DOUBLE; type->size = 8; type->align = 8; break;
        case 8: type->kind = TYPE_LONG_LONG; type->size = 8; type->align = 8; break;
        case 9: type->kind = TYPE_BOOL; type->size = 1; type->align = 1; break;
        default: type->kind = TYPE_INT; type->size = 4; type->align = 4; break;
    }
    
    type->is_unsigned = is_unsigned;
    if (is_const) type->qualifiers |= QUAL_CONST;
    if (is_volatile) type->qualifiers |= QUAL_VOLATILE;
    if (is_restrict) type->qualifiers |= QUAL_RESTRICT;
    type->is_inline = is_inline;
    type->is_noreturn = is_noreturn;
    
    /* Parse pointer */
    while (parse_match(p, TOK_STAR)) {
        mcc_type_t *ptr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
        ptr->kind = TYPE_POINTER;
        ptr->data.pointer.pointee = type;
        type = ptr;
        
        /* Pointer qualifiers */
        while (1) {
            if (parse_match(p, TOK_CONST)) {
                type->qualifiers |= QUAL_CONST;
            } else if (parse_match(p, TOK_VOLATILE)) {
                type->qualifiers |= QUAL_VOLATILE;
            } else if (parse_has_restrict(p) && parse_match(p, TOK_RESTRICT)) {
                type->qualifiers |= QUAL_RESTRICT;
            } else {
                break;
            }
        }
    }
    
    return type;
}

/* ============================================================
 * Abstract Declarator (for casts and sizeof)
 * 
 * C declarator syntax is "inside-out":
 *   int *p         -> pointer to int
 *   int a[10]      -> array of 10 ints
 *   int (*p)[10]   -> pointer to array of 10 ints
 *   int (*f)(int)  -> pointer to function returning int
 * ============================================================ */

/* Parse pointer qualifiers after * */
static mcc_type_qual_t parse_pointer_qualifiers(mcc_parser_t *p)
{
    mcc_type_qual_t quals = QUAL_NONE;
    while (1) {
        if (parse_match(p, TOK_CONST)) {
            quals |= QUAL_CONST;
        } else if (parse_match(p, TOK_VOLATILE)) {
            quals |= QUAL_VOLATILE;
        } else if (parse_has_restrict(p) && parse_match(p, TOK_RESTRICT)) {
            quals |= QUAL_RESTRICT;
        } else {
            break;
        }
    }
    return quals;
}

/* Parse array suffix: [N] or [] or [*] */
static mcc_type_t *parse_array_suffix(mcc_parser_t *p, mcc_type_t *element_type)
{
    size_t arr_size = 0;
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
            mcc_ast_node_t *size_expr = parse_constant_expr(p);
            if (size_expr && size_expr->kind == AST_INT_LIT) {
                arr_size = size_expr->data.int_lit.value;
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
    arr->data.array.element = element_type;
    arr->data.array.length = arr_size;
    arr->data.array.is_vla = is_vla;
    arr->size = element_type->size * arr_size;
    arr->align = element_type->align;
    return arr;
}

/* Parse function parameter list for abstract declarator */
static mcc_type_t *parse_function_suffix(mcc_parser_t *p, mcc_type_t *return_type)
{
    mcc_func_param_t *params = NULL;
    mcc_func_param_t *param_tail = NULL;
    int num_params = 0;
    bool is_variadic = false;
    
    if (!parse_check(p, TOK_RPAREN)) {
        /* Check for (void) - no parameters */
        if (parse_check(p, TOK_VOID)) {
            mcc_token_t *void_tok = p->peek;
            parse_advance(p);
            if (parse_check(p, TOK_RPAREN)) {
                /* (void) means no parameters */
                goto end_params;
            }
            /* void* or similar - parse as parameter type */
            mcc_type_t *param_type = mcc_alloc(p->ctx, sizeof(mcc_type_t));
            param_type->kind = TYPE_VOID;
            param_type = parse_abstract_declarator(p, param_type);
            
            mcc_func_param_t *param = mcc_alloc(p->ctx, sizeof(mcc_func_param_t));
            param->name = NULL;
            param->type = param_type;
            param->next = NULL;
            params = param;
            param_tail = param;
            num_params++;
            
            if (!parse_match(p, TOK_COMMA)) {
                goto end_params;
            }
            (void)void_tok;
        }
        
        do {
            /* Check for ellipsis */
            if (parse_match(p, TOK_ELLIPSIS)) {
                is_variadic = true;
                break;
            }
            
            /* C23: Skip attributes [[...]] on parameters */
            while (parse_check(p, TOK_LBRACKET2)) {
                if (!parse_has_feature(p, MCC_FEAT_ATTR_SYNTAX)) {
                    mcc_warning_at(p->ctx, p->peek->location,
                        "attribute syntax [[...]] is a C23 feature");
                }
                parse_advance(p);  /* Skip [[ */
                int depth = 1;
                while (depth > 0 && !parse_check(p, TOK_EOF)) {
                    if (parse_check(p, TOK_LBRACKET2)) depth++;
                    else if (parse_check(p, TOK_RBRACKET2)) depth--;
                    parse_advance(p);
                }
            }
            
            /* Parse parameter type */
            mcc_type_t *param_base = parse_type_specifier(p);
            
            /* Parse declarator to get both type and optional name */
            const char *param_name = NULL;
            mcc_type_t *param_type = param_base;
            
            /* Check for parameter name (identifier) */
            if (parse_check(p, TOK_IDENT)) {
                param_name = mcc_strdup(p->ctx, p->peek->text);
                parse_advance(p);
                
                /* Parse array brackets - collect all dimensions first */
                size_t array_dims[32];
                int num_dims = 0;
                
                while (parse_check(p, TOK_LBRACKET)) {
                    parse_advance(p);
                    size_t dim = 0;
                    if (!parse_check(p, TOK_RBRACKET)) {
                        if (parse_check(p, TOK_STAR)) {
                            parse_advance(p);  /* VLA [*] */
                        } else {
                            mcc_ast_node_t *size_expr = parse_constant_expr(p);
                            if (size_expr && size_expr->kind == AST_INT_LIT) {
                                dim = size_expr->data.int_lit.value;
                            }
                        }
                    }
                    parse_expect(p, TOK_RBRACKET, "]");
                    if (num_dims < 32) {
                        array_dims[num_dims++] = dim;
                    }
                }
                
                if (num_dims > 0) {
                    /* Build array types from inside out (skip first dimension) */
                    /* First dimension decays to pointer, rest become array types */
                    /* e.g., int m[2][3] becomes int (*)[3] */
                    for (int i = num_dims - 1; i >= 1; i--) {
                        mcc_type_t *arr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                        arr->kind = TYPE_ARRAY;
                        arr->data.array.element = param_type;
                        arr->data.array.length = array_dims[i];
                        arr->data.array.is_vla = false;
                        arr->size = param_type->size * array_dims[i];
                        arr->align = param_type->align;
                        param_type = arr;
                    }
                    /* First dimension decays to pointer */
                    mcc_type_t *ptr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                    ptr->kind = TYPE_POINTER;
                    ptr->data.pointer.pointee = param_type;
                    ptr->size = 8;  /* pointer size */
                    ptr->align = 8;
                    param_type = ptr;
                }
            } else if (parse_check(p, TOK_STAR)) {
                /* Pointer without name - use abstract declarator */
                param_type = parse_abstract_declarator(p, param_base);
            }
            
            mcc_func_param_t *param = mcc_alloc(p->ctx, sizeof(mcc_func_param_t));
            param->name = param_name;
            param->type = param_type;
            param->next = NULL;
            
            if (!params) params = param;
            if (param_tail) param_tail->next = param;
            param_tail = param;
            num_params++;
            
        } while (parse_match(p, TOK_COMMA) && !parse_check(p, TOK_ELLIPSIS));
        
        /* Check for trailing ellipsis */
        if (parse_match(p, TOK_ELLIPSIS)) {
            is_variadic = true;
        }
    }
    
end_params:
    parse_expect(p, TOK_RPAREN, ")");
    
    mcc_type_t *func = mcc_alloc(p->ctx, sizeof(mcc_type_t));
    func->kind = TYPE_FUNCTION;
    func->data.function.return_type = return_type;
    func->data.function.params = params;
    func->data.function.num_params = num_params;
    func->data.function.is_variadic = is_variadic;
    func->data.function.is_oldstyle = false;
    return func;
}

/*
 * Parse abstract-declarator:
 *   pointer
 *   pointer? direct-abstract-declarator
 * 
 * This handles complex types like:
 *   int *           -> pointer to int
 *   int **          -> pointer to pointer to int
 *   int (*)[10]     -> pointer to array of 10 ints
 *   int (*)(int)    -> pointer to function(int) returning int
 *   int *(*)[10]    -> pointer to array of 10 pointers to int
 */
/*
 * Parse a complete declarator (named or abstract)
 * 
 * This is the main entry point for parsing C declarators.
 * It handles the full complexity of C declarator syntax:
 *   int *p           -> pointer to int
 *   int a[10]        -> array of 10 ints
 *   int (*p)[10]     -> pointer to array of 10 ints
 *   int (*f)(int)    -> pointer to function(int) returning int
 *   int *(*arr)[5]   -> pointer to array of 5 pointers to int
 */

/* Forward declaration for recursive parsing */
static parse_declarator_result_t parse_direct_declarator(mcc_parser_t *p, mcc_type_t *base_type, bool allow_abstract);

parse_declarator_result_t parse_declarator(mcc_parser_t *p, mcc_type_t *base_type, bool allow_abstract)
{
    parse_declarator_result_t result = { base_type, NULL };
    
    /* Collect pointer prefixes with their qualifiers */
    typedef struct {
        mcc_type_qual_t quals;
    } ptr_info_t;
    
    ptr_info_t ptr_stack[32];
    int ptr_count = 0;
    
    while (parse_match(p, TOK_STAR)) {
        if (ptr_count < 32) {
            ptr_stack[ptr_count].quals = parse_pointer_qualifiers(p);
            ptr_count++;
        }
    }
    
    /* Parse direct declarator */
    result = parse_direct_declarator(p, base_type, allow_abstract);
    
    /* Apply pointer prefixes in reverse order (innermost first) */
    for (int i = ptr_count - 1; i >= 0; i--) {
        mcc_type_t *ptr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
        ptr->kind = TYPE_POINTER;
        ptr->data.pointer.pointee = result.type;
        ptr->qualifiers = ptr_stack[i].quals;
        result.type = ptr;
    }
    
    return result;
}

/*
 * Parse direct-declarator:
 *   identifier
 *   ( declarator )
 *   direct-declarator [ constant-expression? ]
 *   direct-declarator ( parameter-type-list )
 */
static parse_declarator_result_t parse_direct_declarator(mcc_parser_t *p, mcc_type_t *base_type, bool allow_abstract)
{
    parse_declarator_result_t result = { base_type, NULL };
    
    /* Check for grouped declarator: ( declarator ) */
    if (parse_check(p, TOK_LPAREN)) {
        /* Need to distinguish between:
         * - Grouped declarator: (*p), (*arr)[10], (*f)(int)
         * - Function parameters: (int, int), (void)
         * 
         * It's a grouped declarator if after '(' we see:
         * - '*' (pointer)
         * - identifier followed by ')' or '[' or '('
         * - '(' (nested grouping)
         */
        parse_advance(p); /* consume '(' */
        
        bool is_grouped = false;
        if (parse_check(p, TOK_STAR)) {
            is_grouped = true;
        } else if (parse_check(p, TOK_LPAREN)) {
            is_grouped = true;
        } else if (parse_check(p, TOK_IDENT)) {
            /* Could be grouped declarator with name, or function param */
            /* For now, treat identifier as grouped declarator name */
            is_grouped = true;
        } else if (parse_check(p, TOK_RPAREN) && allow_abstract) {
            /* Empty parens in abstract declarator - function type */
            is_grouped = false;
        }
        
        if (is_grouped) {
            /* Parse inner declarator with placeholder type */
            mcc_type_t *placeholder = mcc_alloc(p->ctx, sizeof(mcc_type_t));
            placeholder->kind = TYPE_VOID;
            
            parse_declarator_result_t inner = parse_declarator(p, placeholder, allow_abstract);
            parse_expect(p, TOK_RPAREN, ")");
            
            /* Parse suffixes that apply to the outer type */
            mcc_type_t *outer_type = base_type;
            
            /* Parse array suffixes */
            while (parse_match(p, TOK_LBRACKET)) {
                outer_type = parse_array_suffix(p, outer_type);
            }
            
            /* Parse function suffix */
            if (parse_match(p, TOK_LPAREN)) {
                outer_type = parse_function_suffix(p, outer_type);
            }
            
            /* Patch the placeholder in the inner type */
            mcc_type_t *curr = inner.type;
            mcc_type_t *prev = NULL;
            while (curr && curr->kind != TYPE_VOID) {
                prev = curr;
                switch (curr->kind) {
                    case TYPE_POINTER:
                        curr = curr->data.pointer.pointee;
                        break;
                    case TYPE_ARRAY:
                        curr = curr->data.array.element;
                        break;
                    case TYPE_FUNCTION:
                        curr = curr->data.function.return_type;
                        break;
                    default:
                        curr = NULL;
                        break;
                }
            }
            
            if (prev) {
                switch (prev->kind) {
                    case TYPE_POINTER:
                        prev->data.pointer.pointee = outer_type;
                        break;
                    case TYPE_ARRAY:
                        prev->data.array.element = outer_type;
                        break;
                    case TYPE_FUNCTION:
                        prev->data.function.return_type = outer_type;
                        break;
                    default:
                        break;
                }
                result.type = inner.type;
            } else {
                result.type = outer_type;
            }
            result.name = inner.name;
            return result;
        } else {
            /* Function parameters */
            result.type = parse_function_suffix(p, base_type);
            return result;
        }
    }
    
    /* Parse identifier (if present and not abstract) */
    if (parse_check(p, TOK_IDENT)) {
        result.name = mcc_strdup(p->ctx, p->peek->text);
        parse_advance(p);
    } else if (!allow_abstract) {
        mcc_error_at(p->ctx, p->peek->location, "expected identifier");
    }
    
    /* Parse array and function suffixes */
    /* For arrays, we need to collect all dimensions first, then build types from inside out */
    /* e.g., int arr[3][4] means arr is array[3] of array[4] of int */
    /* So we build: int -> int[4] -> int[3][4] */
    size_t array_dims[32];
    int num_dims = 0;
    
    while (1) {
        if (parse_check(p, TOK_LBRACKET)) {
            parse_advance(p);
            size_t dim = 0;
            if (!parse_check(p, TOK_RBRACKET)) {
                mcc_ast_node_t *size_expr = parse_constant_expr(p);
                if (size_expr && size_expr->kind == AST_INT_LIT) {
                    dim = size_expr->data.int_lit.value;
                }
            }
            parse_expect(p, TOK_RBRACKET, "]");
            if (num_dims < 32) {
                array_dims[num_dims++] = dim;
            }
        } else if (parse_match(p, TOK_LPAREN)) {
            result.type = parse_function_suffix(p, result.type);
        } else {
            break;
        }
    }
    
    /* Build array types from inside out (reverse order) */
    for (int i = num_dims - 1; i >= 0; i--) {
        mcc_type_t *arr = mcc_alloc(p->ctx, sizeof(mcc_type_t));
        arr->kind = TYPE_ARRAY;
        arr->data.array.element = result.type;
        arr->data.array.length = array_dims[i];
        arr->data.array.is_vla = false;
        arr->size = result.type->size * array_dims[i];
        arr->align = result.type->align;
        result.type = arr;
    }
    
    return result;
}

/* Wrapper for backward compatibility */
mcc_type_t *parse_abstract_declarator(mcc_parser_t *p, mcc_type_t *base_type)
{
    parse_declarator_result_t result = parse_declarator(p, base_type, true);
    return result.type;
}
