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
        
        /* C11: _Noreturn, _Thread_local */
        case TOK__NORETURN:
            return parse_has_noreturn(p);
        case TOK__THREAD_LOCAL:
            return parse_has_thread_local(p);
        
        /* C11: _Alignas */
        case TOK__ALIGNAS:
            return parse_has_alignas(p);
        
        /* C11: _Static_assert */
        case TOK__STATIC_ASSERT:
            return parse_has_static_assert(p);
        
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
    
    if (parse_match(p, TOK_LBRACE)) {
        /* Parse enumerators */
        int64_t next_value = 0;
        
        while (!parse_check(p, TOK_RBRACE) && !parse_check(p, TOK_EOF)) {
            mcc_token_t *name_tok = parse_expect(p, TOK_IDENT, "enumerator name");
            
            int64_t value = next_value;
            if (parse_match(p, TOK_ASSIGN)) {
                mcc_ast_node_t *val_expr = parse_constant_expr(p);
                if (val_expr && val_expr->kind == AST_INT_LIT) {
                    value = val_expr->data.int_lit.value;
                }
            }
            
            /* Register enum constant in symbol table */
            /* TODO: Proper enum constant handling */
            (void)name_tok;
            
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
                break;
            case TOK_INT:
                parse_advance(p);
                type_spec = 4;
                break;
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
        case 1: type->kind = TYPE_VOID; break;
        case 2: type->kind = TYPE_CHAR; break;
        case 3: type->kind = TYPE_SHORT; break;
        case 4: type->kind = TYPE_INT; break;
        case 5: type->kind = TYPE_LONG; break;
        case 6: type->kind = TYPE_FLOAT; break;
        case 7: type->kind = TYPE_DOUBLE; break;
        case 8: type->kind = TYPE_LONG_LONG; break;
        case 9: type->kind = TYPE_BOOL; break;
        default: type->kind = TYPE_INT; break;
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
 * ============================================================ */

mcc_type_t *parse_abstract_declarator(mcc_parser_t *p, mcc_type_t *base_type)
{
    mcc_type_t *type = base_type;
    
    /* Parse pointers */
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
    
    /* TODO: Parse array and function declarators */
    
    return type;
}
