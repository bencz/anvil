/*
 * MCC - Micro C Compiler
 * Parser Main Module
 * 
 * This file contains the public API and core token operations
 * for the parser. The actual parsing logic is split into:
 * - parse_expr.c  - Expression parsing
 * - parse_stmt.c  - Statement parsing
 * - parse_type.c  - Type parsing
 * - parse_decl.c  - Declaration parsing
 */

#include "parse_internal.h"

/* ============================================================
 * Parser Creation/Destruction
 * ============================================================ */

mcc_parser_t *mcc_parser_create(mcc_context_t *ctx, mcc_preprocessor_t *pp)
{
    mcc_parser_t *p = mcc_alloc(ctx, sizeof(mcc_parser_t));
    p->ctx = ctx;
    p->pp = pp;
    p->current = NULL;
    p->peek = NULL;
    p->symtab = NULL;
    p->struct_types = NULL;
    p->typedefs = NULL;
    p->panic_mode = false;
    p->sync_depth = 0;
    return p;
}

void mcc_parser_destroy(mcc_parser_t *p)
{
    (void)p; /* Arena allocated */
}

/* ============================================================
 * Token Operations
 * ============================================================ */

mcc_token_t *parse_advance(mcc_parser_t *p)
{
    p->current = p->peek;
    p->peek = mcc_preprocessor_next(p->pp);
    
    /* Skip newlines */
    while (p->peek->type == TOK_NEWLINE) {
        p->peek = mcc_preprocessor_next(p->pp);
    }
    
    return p->current;
}

bool parse_check(mcc_parser_t *p, mcc_token_type_t type)
{
    return p->peek->type == type;
}

bool parse_match(mcc_parser_t *p, mcc_token_type_t type)
{
    if (parse_check(p, type)) {
        parse_advance(p);
        return true;
    }
    return false;
}

mcc_token_t *parse_expect(mcc_parser_t *p, mcc_token_type_t type, const char *msg)
{
    if (parse_check(p, type)) {
        return parse_advance(p);
    }
    
    mcc_error_at(p->ctx, p->peek->location, "expected %s, got '%s'",
                 msg ? msg : mcc_token_type_name(type),
                 mcc_token_to_string(p->peek));
    p->panic_mode = true;
    return p->peek;
}

/* ============================================================
 * Error Recovery
 * ============================================================ */

void parse_synchronize(mcc_parser_t *p)
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
        
        parse_advance(p);
    }
}

/* ============================================================
 * Public API - Wrapper functions for backward compatibility
 * ============================================================ */

mcc_token_t *mcc_parser_advance(mcc_parser_t *p)
{
    return parse_advance(p);
}

bool mcc_parser_check(mcc_parser_t *p, mcc_token_type_t type)
{
    return parse_check(p, type);
}

bool mcc_parser_match(mcc_parser_t *p, mcc_token_type_t type)
{
    return parse_match(p, type);
}

mcc_token_t *mcc_parser_expect(mcc_parser_t *p, mcc_token_type_t type, const char *msg)
{
    return parse_expect(p, type, msg);
}

void mcc_parser_synchronize(mcc_parser_t *p)
{
    parse_synchronize(p);
}

/* ============================================================
 * Main Parsing Entry Points
 * ============================================================ */

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
    
    while (!parse_check(p, TOK_EOF)) {
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
            parse_synchronize(p);
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
    while (p->peek->type == TOK_NEWLINE) {
        p->peek = mcc_preprocessor_next(p->pp);
    }
    return parse_expression(p);
}

mcc_ast_node_t *mcc_parser_parse_statement(mcc_parser_t *p)
{
    p->peek = mcc_preprocessor_next(p->pp);
    while (p->peek->type == TOK_NEWLINE) {
        p->peek = mcc_preprocessor_next(p->pp);
    }
    return parse_statement(p);
}

mcc_ast_node_t *mcc_parser_parse_declaration(mcc_parser_t *p)
{
    p->peek = mcc_preprocessor_next(p->pp);
    while (p->peek->type == TOK_NEWLINE) {
        p->peek = mcc_preprocessor_next(p->pp);
    }
    return parse_declaration(p);
}
