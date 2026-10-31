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
#include <limits.h>

static mcc_type_t *parse_primitive_layout(mcc_parser_t *p, int type_spec,
                                          int long_count, bool is_signed,
                                          bool is_unsigned)
{
    mcc_type_t *base;
    switch (type_spec) {
        case 1: base = mcc_type_void(p->types); break;
        case 2:
            base = is_unsigned ? mcc_type_uchar(p->types)
                : is_signed ? mcc_type_schar(p->types)
                : mcc_type_char(p->types);
            break;
        case 3: base = is_unsigned ? mcc_type_ushort(p->types)
                                   : mcc_type_short(p->types); break;
        case 4: base = is_unsigned ? mcc_type_uint(p->types)
                                   : mcc_type_int(p->types); break;
        case 5: base = is_unsigned ? mcc_type_ulong(p->types)
                                   : mcc_type_long(p->types); break;
        case 6: base = mcc_type_float(p->types); break;
        case 7: base = long_count > 0 ? mcc_type_long_double(p->types)
                                     : mcc_type_double(p->types); break;
        case 8: base = is_unsigned ? mcc_type_ullong(p->types)
                                   : mcc_type_llong(p->types); break;
        case 9: base = mcc_type_bool(p->types); break;
        default: base = mcc_type_int(p->types); break;
    }
    if (!base) return NULL;
    mcc_type_t *copy = mcc_alloc(p->ctx, sizeof(*copy));
    if (!copy) return NULL;
    *copy = *base;
    return copy;
}

static mcc_type_t *parse_pointer_type(mcc_parser_t *p, mcc_type_t *pointee)
{
    return mcc_type_pointer(p->types, pointee);
}

static mcc_type_t *parse_array_type(mcc_parser_t *p, mcc_type_t *element,
                                    size_t length)
{
    return mcc_type_array(p->types, element, length);
}

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
        
        /* C23: typeof, typeof_unqual. The lexer also maps GNU's
         * __typeof__ to TOK_TYPEOF, so we accept either the C23 flag
         * or the GNU-typeof flag. */
        case TOK_TYPEOF:
        case TOK_TYPEOF_UNQUAL:
            return parse_has_feature(p, MCC_FEAT_TYPEOF) ||
                   parse_has_feature(p, MCC_FEAT_GNU_TYPEOF);
        
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
        
        /* C23: Attribute syntax [[...]] - could be start of declaration */
        case TOK_LBRACKET2:
            return true;
            
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
        stype->data.record.static_asserts = NULL;
        stype->data.record.num_fields = 0;
        
        /* Parse fields */
        mcc_struct_field_t *fields = NULL;
        mcc_struct_field_t *field_tail = NULL;
        mcc_record_assert_t *static_asserts = NULL;
        mcc_record_assert_t *static_assert_tail = NULL;
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
                
                mcc_record_assert_t *record_assert = mcc_alloc(
                    p->ctx, sizeof(*record_assert));
                record_assert->expr = expr;
                record_assert->message = mcc_strdup(
                    p->ctx, msg->literal.string_val.value);
                record_assert->location = expr ? expr->location : loc;
                if (!static_asserts) {
                    static_asserts = static_assert_tail = record_assert;
                } else {
                    static_assert_tail->next = record_assert;
                    static_assert_tail = record_assert;
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
                if (num_fields >= PARSE_MAX_FIELDS) {
                    mcc_error_at(p->ctx, loc,
                                 "record exceeds %d fields", PARSE_MAX_FIELDS);
                    return NULL;
                }
                mcc_struct_field_t *field = mcc_alloc(p->ctx, sizeof(mcc_struct_field_t));
                if (!field) return NULL;
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
                const char *field_name_str = NULL;
                
                /* Check for function pointer: type (*name)(params) */
                if (parse_check(p, TOK_LPAREN) && p->peek->type == TOK_LPAREN) {
                    parse_advance(p); /* consume '(' */
                    if (parse_match(p, TOK_STAR)) {
                        /* This is a function pointer */
                        if (parse_check(p, TOK_IDENT)) {
                            field_name_str = mcc_strdup(p->ctx, p->peek->text);
                            parse_advance(p);
                        }
                        parse_expect(p, TOK_RPAREN, ")");
                        
                        /* Parse function parameters */
                        parse_expect(p, TOK_LPAREN, "(");
                        /* Skip parameters for now - just match parens */
                        int paren_depth = 1;
                        while (paren_depth > 0 && !parse_check(p, TOK_EOF)) {
                            if (parse_check(p, TOK_LPAREN)) paren_depth++;
                            else if (parse_check(p, TOK_RPAREN)) paren_depth--;
                            if (paren_depth > 0) parse_advance(p);
                        }
                        parse_expect(p, TOK_RPAREN, ")");
                        
                        /* Create function pointer type */
                        mcc_type_t *func_type = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                        func_type->kind = TYPE_FUNCTION;
                        func_type->data.function.return_type = field_base_type;
                        func_type->data.function.params = NULL;
                        func_type->data.function.num_params = 0;
                        func_type->data.function.is_variadic = false;
                        
                        field_type = parse_pointer_type(p, func_type);
                        
                        goto add_field;
                    } else {
                        /* Not a function pointer, backtrack - this shouldn't happen in valid C */
                        mcc_error_at(p->ctx, loc, "unexpected '(' in struct field");
                    }
                }
                
                while (parse_match(p, TOK_STAR)) {
                    field_type = parse_pointer_type(p, field_type);
                    
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
                
                /* Check for anonymous bitfield (e.g., "unsigned int : 5;") */
                if (parse_check(p, TOK_IDENT)) {
                    field_name_str = mcc_strdup(p->ctx, p->peek->text);
                    parse_advance(p);
                } else if (!parse_check(p, TOK_COLON)) {
                    /* Not an anonymous bitfield - error */
                    parse_expect(p, TOK_IDENT, "field name");
                }
                /* If we're here with field_name_str == NULL, it's an anonymous bitfield */
                
                /* Parse array brackets for this field.  Dimensions are
                 * collected first and applied inside-out: int m[2][3] is
                 * an array of 2 arrays of 3 ints. */
                size_t field_dims[32];
                bool field_flexible[32];
                int field_num_dims = 0;
                while (parse_match(p, TOK_LBRACKET)) {
                    size_t arr_size = 0;
                    bool is_flexible = false;
                    
                    if (!parse_check(p, TOK_RBRACKET)) {
                        mcc_ast_node_t *size_expr = parse_expression(p);
                        int64_t bound;
                        if (size_expr && parse_eval_int_constant(p, size_expr, &bound) &&
                            bound >= 0) {
                            arr_size = (size_t)bound;
                        } else {
                            mcc_error_at(p->ctx, loc,
                                         "struct member array size must be a "
                                         "non-negative integer constant expression");
                        }
                    } else {
                        /* C99: Flexible array member */
                        is_flexible = true;
                        if (!parse_has_flexible_array(p)) {
                            mcc_warning_at(p->ctx, loc,
                                "flexible array members are a C99 extension");
                        }
                    }
                    parse_expect(p, TOK_RBRACKET, "]");
                    if (field_num_dims == 32) {
                        mcc_error_at(p->ctx, loc,
                                     "array declarator exceeds 32 dimensions");
                        return NULL;
                    }
                    field_dims[field_num_dims] = arr_size;
                    field_flexible[field_num_dims] = is_flexible;
                    field_num_dims++;
                }
                for (int dim = field_num_dims - 1; dim >= 0; dim--) {
                    mcc_type_t *arr = parse_array_type(p, field_type, field_dims[dim]);
                    if (!arr) return NULL;
                    arr->data.array.is_flexible = field_flexible[dim];
                    field_type = arr;
                }
                
                /* Parse bitfield width */
                int bitfield_width = 0;
                if (parse_match(p, TOK_COLON)) {
                    mcc_error_at(p->ctx, loc,
                                 "bit-fields are not implemented by MCC");
                    mcc_ast_node_t *width_expr = parse_constant_expr(p);
                    if (width_expr && width_expr->kind == AST_INT_LIT) {
                        bitfield_width = (int)width_expr->data.int_lit.value;
                        if (bitfield_width < 0) {
                            mcc_error_at(p->ctx, loc,
                                "bitfield width cannot be negative");
                            bitfield_width = 0;
                        } else if (bitfield_width > 64) {
                            mcc_error_at(p->ctx, loc,
                                "bitfield width exceeds maximum (64 bits)");
                            bitfield_width = 64;
                        }
                    } else {
                        mcc_error_at(p->ctx, loc,
                            "bitfield width must be a constant expression");
                    }
                }
                
            add_field: ;
                if (num_fields >= PARSE_MAX_FIELDS) {
                    mcc_error_at(p->ctx, loc,
                                 "record exceeds %d fields", PARSE_MAX_FIELDS);
                    return NULL;
                }
                mcc_struct_field_t *field = mcc_alloc(p->ctx, sizeof(mcc_struct_field_t));
                if (!field) return NULL;
                field->name = field_name_str;
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

        stype->data.record.static_asserts = static_asserts;
        if (is_union) {
            if (!mcc_type_complete_union(p->types, stype, fields,
                                         num_fields)) return NULL;
        } else {
            if (!mcc_type_complete_struct(p->types, stype, fields,
                                          num_fields)) return NULL;
        }
        
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
            stype->data.record.static_asserts = NULL;
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
        tag = p->peek->text;
        parse_advance(p);
    }
    
    /* Never bypass the target-aware type constructor: enum has the selected
     * target's exact int layout, including its ABI alignment. */
    mcc_type_t *etype = mcc_type_enum(p->types, tag);
    if (!etype) return NULL;
    
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
                if (!val_expr || !parse_eval_int_constant(p, val_expr, &value)) {
                    mcc_error_at(p->ctx, name_tok->location,
                                 "enumerator value for '%s' is not an integer "
                                 "constant expression", name_tok->text);
                    value = next_value;
                }
            }
            if (value < INT_MIN || value > INT_MAX) {
                mcc_error_at(p->ctx, name_tok->location,
                             "enumerator value is not representable as int");
            }
            
            /* Create enum constant */
            mcc_enum_const_t *econst = mcc_alloc(p->ctx, sizeof(mcc_enum_const_t));
            if (!econst) return etype;
            econst->name = mcc_strdup(p->ctx, name_tok->text);
            if (!econst->name) return etype;
            econst->value = value;
            econst->next = NULL;

            /* Later bounds and enumerators may refer to it. */
            mcc_enum_entry_t *visible = mcc_alloc(p->ctx, sizeof(*visible));
            if (visible) {
                visible->name = econst->name;
                visible->value = value;
                visible->depth = p->typedef_depth;
                visible->next = p->enumerators;
                p->enumerators = visible;
            }
            
            if (!constants) constants = econst;
            if (const_tail) const_tail->next = econst;
            const_tail = econst;
            if (num_constants == INT_MAX) {
                mcc_fatal(p->ctx, "enum constant count overflow");
                return etype;
            }
            num_constants++;
            
            if (value == INT_MAX) {
                next_value = (int64_t)INT_MAX + 1;
            } else {
                next_value = value + 1;
            }
            
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
    bool is_atomic = false;
    bool is_inline = false;
    bool is_noreturn = false;
    bool is_complex = false;
    bool is_imaginary = false;
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
                    mcc_error_at(p->ctx, p->peek->location,
                        "'_Noreturn' semantics are not implemented by MCC");
                }
                parse_advance(p);
                is_noreturn = true;
                continue;
            case TOK__ATOMIC:
                if (!parse_has_atomic(p)) {
                    mcc_error_at(p->ctx, p->peek->location,
                        "atomic semantics are not implemented by MCC");
                }
                parse_advance(p);
                /* _Atomic has two forms:
                 *   _Atomic type-name           (qualifier)
                 *   _Atomic(type-name)          (specifier — returns the
                 *                                qualified version of the
                 *                                inner type directly)
                 * For the parenthesised form we consume the inner type and
                 * return it qualified so the surrounding code sees a real
                 * type (previously it was silently discarded). */
                if (parse_match(p, TOK_LPAREN)) {
                    mcc_type_t *inner = parse_type_specifier(p);
                    parse_expect(p, TOK_RPAREN, ")");
                    if (inner) {
                        mcc_type_t *qinner = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                        *qinner = *inner;
                        qinner->qualifiers |= QUAL_ATOMIC;
                        return qinner;
                    }
                }
                is_atomic = true;
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
                continue;  /* Allow 'float _Complex' */
            case TOK_DOUBLE:
                parse_advance(p);
                type_spec = 7;
                continue;  /* Allow 'double _Complex' */
            case TOK__BOOL:
                if (!parse_has_bool(p)) {
                    mcc_warning_at(p->ctx, p->peek->location,
                        "'_Bool' is a C99 extension");
                }
                parse_advance(p);
                type_spec = 9; /* _Bool */
                break;
            case TOK__COMPLEX:
                if (!parse_has_feature(p, MCC_FEAT_COMPLEX)) {
                    mcc_error_at(p->ctx, p->current->location,
                        "'_Complex' is not implemented by MCC");
                }
                parse_advance(p);
                is_complex = true;
                continue;
            case TOK__IMAGINARY:
                if (!parse_has_feature(p, MCC_FEAT_IMAGINARY)) {
                    mcc_error_at(p->ctx, p->current->location,
                        "'_Imaginary' is not implemented by MCC");
                }
                parse_advance(p);
                is_imaginary = true;
                continue;
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
                if (!parse_has_feature(p, MCC_FEAT_TYPEOF) &&
                    !parse_has_feature(p, MCC_FEAT_GNU_TYPEOF)) {
                    mcc_warning_at(p->ctx, p->peek->location,
                        "'typeof' is a C23 extension");
                }
                parse_advance(p);
                parse_expect(p, TOK_LPAREN, "(");
                
                mcc_type_t *result_type;
                if (parse_is_type_start(p)) {
                    result_type = parse_type_specifier(p);
                } else {
                    /* typeof(expression) is unevaluated, but its type cannot
                     * generally be known until semantic analysis has populated
                     * the surrounding scope. Keep an explicit deferred type
                     * instead of the old (and wrong) unconditional `int`. */
                    mcc_ast_node_t *expr = parse_expression(p);
                    result_type = mcc_alloc(p->ctx, sizeof(mcc_type_t));
                    result_type->kind = TYPE_TYPEOF_EXPR;
                    result_type->data.typeof_expr.expr = expr;
                    result_type->data.typeof_expr.unqualified = is_unqual;
                }
                parse_expect(p, TOK_RPAREN, ")");
                
                /* typeof_unqual removes qualifiers */
                if (is_unqual && result_type &&
                    result_type->kind != TYPE_TYPEOF_EXPR) {
                    result_type->qualifiers = 0;
                }
                
                /* Handle additional qualifiers */
                if (is_const && result_type) result_type->qualifiers |= QUAL_CONST;
                if (is_volatile && result_type) result_type->qualifiers |= QUAL_VOLATILE;
                if (is_atomic && result_type) result_type->qualifiers |= QUAL_ATOMIC;
                
                return result_type;
            }
            case TOK_STRUCT:
            case TOK_UNION: {
                bool is_union = (p->peek->type == TOK_UNION);
                mcc_type_t *stype = parse_struct_or_union(p, is_union);
                
                /* Handle qualifiers */
                if (is_const) stype->qualifiers |= QUAL_CONST;
                if (is_volatile) stype->qualifiers |= QUAL_VOLATILE;
                if (is_atomic) stype->qualifiers |= QUAL_ATOMIC;
                
                /* Parse pointer */
                while (parse_match(p, TOK_STAR)) {
                    stype = parse_pointer_type(p, stype);
                    
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
                if (is_atomic) etype->qualifiers |= QUAL_ATOMIC;
                
                /* Parse pointer */
                while (parse_match(p, TOK_STAR)) {
                    etype = parse_pointer_type(p, etype);
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
                            mcc_type_qual_t added = QUAL_NONE;
                            if (is_const) added |= QUAL_CONST;
                            if (is_volatile) added |= QUAL_VOLATILE;
                            if (is_atomic) added |= QUAL_ATOMIC;
                            if (added) type = mcc_type_qualified(
                                p->types, type, type->qualifiers | added);
                            
                            /* Parse pointer */
                            while (parse_match(p, TOK_STAR)) {
                                type = parse_pointer_type(p, type);
                                
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
    
    /* Default to double if only _Complex/_Imaginary */
    if (type_spec == 0 && (is_complex || is_imaginary)) {
        type_spec = 7; /* double _Complex by default */
    }
    
    /* Create a type whose primitive layout comes from the selected target's
     * Anvil DataLayout. */
    mcc_type_t *type = NULL;
    
    /* Handle _Complex types */
    if (is_complex) {
        mcc_type_t *base = type_spec == 6
            ? mcc_type_complex_float(p->types)
            : long_count > 0
                ? mcc_type_complex_ldouble(p->types)
                : mcc_type_complex_double(p->types);
        type = mcc_alloc(p->ctx, sizeof(*type));
        if (!type || !base) return NULL;
        *type = *base;
    } else {
        type = parse_primitive_layout(p, type_spec, long_count,
                                      is_signed, is_unsigned);
        if (!type) return NULL;
    }
    
    /* Handle long double _Complex specially */
    if (is_complex && long_count > 0 && type_spec == 7) {
        type->kind = TYPE_COMPLEX_LDOUBLE;
    }
    if (!is_complex && long_count > 0 && type_spec == 7) {
        mcc_error_at(p->ctx, p->current ? p->current->location
                                        : p->peek->location,
                     "'long double' ABI lowering is not implemented by MCC");
    }
    
    if (mcc_type_is_integer(type)) type->is_unsigned = is_unsigned;
    if (is_const) type->qualifiers |= QUAL_CONST;
    if (is_volatile) type->qualifiers |= QUAL_VOLATILE;
    if (is_restrict) type->qualifiers |= QUAL_RESTRICT;
    if (is_atomic) type->qualifiers |= QUAL_ATOMIC;
    type->is_inline = is_inline;
    type->is_noreturn = is_noreturn;
    
    /* Parse pointer */
    while (parse_match(p, TOK_STAR)) {
        type = parse_pointer_type(p, type);
        
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
/*
 * Parse every array suffix [..][..]... following a declarator and build the
 * array type inside out: T x[2][3] is an array of 2 arrays of 3 T.  A
 * runtime bound makes a VLA (its expression kept for the codegen); [*] is
 * a VLA of unspecified size; `static` and qualifiers inside the brackets
 * (parameter declarators) are accepted.  Returns element_type unchanged
 * when no suffix follows.
 */
static mcc_type_t *parse_array_dimensions(mcc_parser_t *p, mcc_type_t *element_type)
{
    size_t dims[32];
    bool dim_is_vla[32];
    mcc_ast_node_t *dim_exprs[32];
    int num_dims = 0;

    while (parse_match(p, TOK_LBRACKET)) {
        while (parse_match(p, TOK_STATIC) || parse_match(p, TOK_CONST) ||
               parse_match(p, TOK_VOLATILE) || parse_match(p, TOK_RESTRICT)) {
        }

        size_t dim = 0;
        bool is_vla = false;
        mcc_ast_node_t *length_expr = NULL;
        if (parse_check(p, TOK_STAR)) {
            parse_advance(p);
            is_vla = true;
        } else if (!parse_check(p, TOK_RBRACKET)) {
            mcc_ast_node_t *size_expr = parse_constant_expr(p);
            int64_t bound;
            if (size_expr && parse_eval_int_constant(p, size_expr, &bound)) {
                if (bound < 0) {
                    mcc_error_at(p->ctx, size_expr->location, "array size is negative");
                    bound = 0;
                }
                dim = (size_t)bound;
            } else if (size_expr) {
                /* Non-constant bound: a VLA whose count the codegen
                 * evaluates at run time. */
                is_vla = true;
                length_expr = size_expr;
            }
        }
        if (is_vla && !parse_has_vla(p))
            mcc_warning_at(p->ctx, p->peek->location, "variable length arrays are a C99 extension");
        parse_expect(p, TOK_RBRACKET, "]");

        if (num_dims >= 32) {
            mcc_error_at(p->ctx, p->peek->location, "array declarator exceeds 32 dimensions");
            return NULL;
        }
        dims[num_dims] = dim;
        dim_is_vla[num_dims] = is_vla;
        dim_exprs[num_dims] = length_expr;
        num_dims++;
    }

    mcc_type_t *type = element_type;
    for (int i = num_dims - 1; i >= 0 && type; i--) {
        mcc_type_t *arr = parse_array_type(p, type, dims[i]);
        if (!arr)
            return NULL;
        arr->data.array.is_vla = dim_is_vla[i];
        arr->data.array.length_expr = dim_exprs[i];
        if (dim_is_vla[i])
            arr->size = 0;
        type = arr;
    }
    return type;
}

/* One parameter: its declarator (named or abstract) with the C parameter
 * adjustments, array of T -> pointer to T and function -> pointer to
 * function (C11 6.7.6.3p7-8).  The adjusted pointer keeps the inner array
 * types, VLA bounds included. */
static mcc_func_param_t *parse_parameter(mcc_parser_t *p, mcc_type_t *base_type)
{
    parse_declarator_result_t declarator = parse_declarator(p, base_type, true);
    mcc_type_t *type = declarator.type;
    if (!type)
        return NULL;
    if (type->kind == TYPE_ARRAY)
        type = parse_pointer_type(p, type->data.array.element);
    else if (type->kind == TYPE_FUNCTION)
        type = parse_pointer_type(p, type);
    if (!type)
        return NULL;

    mcc_func_param_t *param = mcc_alloc(p->ctx, sizeof(mcc_func_param_t));
    if (!param)
        return NULL;
    param->name = declarator.name;
    param->type = type;
    param->next = NULL;
    return param;
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
            /* void *p and the like */
            mcc_type_t *void_type = mcc_alloc(p->ctx, sizeof(mcc_type_t));
            if (!void_type) return NULL;
            void_type->kind = TYPE_VOID;
            mcc_func_param_t *param = parse_parameter(p, void_type);
            if (!param) return NULL;
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
                    mcc_error_at(p->ctx, p->peek->location,
                        "C23 parameter attributes are not implemented by MCC");
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
            
            mcc_func_param_t *param = parse_parameter(p, param_base);
            if (!param) return NULL;
            
            if (!params) params = param;
            if (param_tail) param_tail->next = param;
            param_tail = param;
            num_params++;
            if (num_params > PARSE_MAX_PARAMS) {
                mcc_error_at(p->ctx, p->peek->location,
                             "function exceeds %d parameters",
                             PARSE_MAX_PARAMS);
                return NULL;
            }
            
        } while (parse_match(p, TOK_COMMA) && !parse_check(p, TOK_ELLIPSIS));
        
        /* Check for trailing ellipsis */
        if (parse_match(p, TOK_ELLIPSIS)) {
            is_variadic = true;
        }
    }
    
end_params:
    parse_expect(p, TOK_RPAREN, ")");
    
    mcc_type_t *func = mcc_type_function(p->types, return_type, params, num_params, is_variadic);
    if (!func)
        return NULL;

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
    
    /* Pointer prefixes bind looser than the direct declarator's suffixes:
     * they wrap the base type (the first '*' innermost), and the suffixes
     * then apply to that, so int *a[4] is an array of pointers. */
    mcc_type_t *type = base_type;
    for (int i = 0; i < ptr_count; i++) {
        mcc_type_t *ptr = parse_pointer_type(p, type);
        if (!ptr) return (parse_declarator_result_t){ NULL, NULL };
        ptr->qualifiers = ptr_stack[i].quals;
        type = ptr;
    }

    return parse_direct_declarator(p, type, allow_abstract);
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
            outer_type = parse_array_dimensions(p, outer_type);
            if (!outer_type)
                return (parse_declarator_result_t){ NULL, inner.name };
            
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
    
    /* Function suffixes, then the array dimensions (built inside out). */
    while (parse_match(p, TOK_LPAREN))
        result.type = parse_function_suffix(p, result.type);
    result.type = parse_array_dimensions(p, result.type);
    return result;
}

/* Wrapper for backward compatibility */
mcc_type_t *parse_abstract_declarator(mcc_parser_t *p, mcc_type_t *base_type)
{
    parse_declarator_result_t result = parse_declarator(p, base_type, true);
    return result.type;
}
