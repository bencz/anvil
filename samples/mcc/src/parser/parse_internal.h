/*
 * MCC - Micro C Compiler
 * Parser Internal Header
 * 
 * This file contains internal structures, constants, and function
 * declarations used by the parser implementation.
 */

#ifndef MCC_PARSE_INTERNAL_H
#define MCC_PARSE_INTERNAL_H

#include "mcc.h"

/* ============================================================
 * Constants
 * ============================================================ */

#define PARSE_MAX_PARAMS    256
#define PARSE_MAX_FIELDS    1024
#define PARSE_MAX_ARGS      256

/* ============================================================
 * Token Operations (parser.c)
 * ============================================================ */

mcc_token_t *parse_advance(mcc_parser_t *p);
bool parse_check(mcc_parser_t *p, mcc_token_type_t type);
bool parse_match(mcc_parser_t *p, mcc_token_type_t type);
mcc_token_t *parse_expect(mcc_parser_t *p, mcc_token_type_t type, const char *msg);
void parse_synchronize(mcc_parser_t *p);

/* ============================================================
 * Type Parsing (parse_type.c)
 * ============================================================ */

/* Check if current token starts a type */
bool parse_is_type_start(mcc_parser_t *p);

/* Check if current token starts a declaration */
bool parse_is_declaration_start(mcc_parser_t *p);

/* Check if identifier is a typedef name */
bool parse_is_typedef_name(mcc_parser_t *p, const char *name);

/* Parse type specifier */
mcc_type_t *parse_type_specifier(mcc_parser_t *p);

/* Parse abstract declarator (for casts and sizeof) */
mcc_type_t *parse_abstract_declarator(mcc_parser_t *p, mcc_type_t *base_type);

/* ============================================================
 * Expression Parsing (parse_expr.c)
 * ============================================================ */

/* Parse primary expression (literals, identifiers, parenthesized) */
mcc_ast_node_t *parse_primary(mcc_parser_t *p);

/* Parse postfix expression ([], (), ., ->, ++, --) */
mcc_ast_node_t *parse_postfix(mcc_parser_t *p);

/* Parse unary expression (++, --, &, *, +, -, ~, !) */
mcc_ast_node_t *parse_unary(mcc_parser_t *p);

/* Parse binary expression with precedence climbing */
mcc_ast_node_t *parse_binary(mcc_parser_t *p, int min_prec);

/* Parse ternary expression (?:) */
mcc_ast_node_t *parse_ternary(mcc_parser_t *p);

/* Parse assignment expression */
mcc_ast_node_t *parse_assignment_expr(mcc_parser_t *p);

/* Parse comma expression */
mcc_ast_node_t *parse_expression(mcc_parser_t *p);

/* Parse constant expression (for array sizes, case labels, etc.) */
mcc_ast_node_t *parse_constant_expr(mcc_parser_t *p);

/* ============================================================
 * Statement Parsing (parse_stmt.c)
 * ============================================================ */

/* Parse compound statement { ... } */
mcc_ast_node_t *parse_compound_stmt(mcc_parser_t *p);

/* Parse any statement */
mcc_ast_node_t *parse_statement(mcc_parser_t *p);

/* Parse if statement */
mcc_ast_node_t *parse_if_stmt(mcc_parser_t *p);

/* Parse while statement */
mcc_ast_node_t *parse_while_stmt(mcc_parser_t *p);

/* Parse do-while statement */
mcc_ast_node_t *parse_do_stmt(mcc_parser_t *p);

/* Parse for statement */
mcc_ast_node_t *parse_for_stmt(mcc_parser_t *p);

/* Parse switch statement */
mcc_ast_node_t *parse_switch_stmt(mcc_parser_t *p);

/* Parse return statement */
mcc_ast_node_t *parse_return_stmt(mcc_parser_t *p);

/* Parse goto statement */
mcc_ast_node_t *parse_goto_stmt(mcc_parser_t *p);

/* Parse labeled statement (label:, case:, default:) */
mcc_ast_node_t *parse_labeled_stmt(mcc_parser_t *p);

/* ============================================================
 * Declaration Parsing (parse_decl.c)
 * ============================================================ */

/* Parse any declaration (variable, function, typedef, struct, etc.) */
mcc_ast_node_t *parse_declaration(mcc_parser_t *p);

/* Parse function declaration/definition */
mcc_ast_node_t *parse_function_decl(mcc_parser_t *p, mcc_type_t *base_type, 
                                     const char *name, mcc_storage_class_t storage,
                                     mcc_location_t loc);

/* Parse variable declaration */
mcc_ast_node_t *parse_variable_decl(mcc_parser_t *p, mcc_type_t *base_type,
                                     const char *name, mcc_storage_class_t storage,
                                     bool is_typedef, mcc_location_t loc);

/* Parse struct/union definition */
mcc_type_t *parse_struct_or_union(mcc_parser_t *p, bool is_union);

/* Parse enum definition */
mcc_type_t *parse_enum(mcc_parser_t *p);

/* Parse initializer (expression or { ... }) */
mcc_ast_node_t *parse_initializer(mcc_parser_t *p);

/* ============================================================
 * C Standard Feature Checks for Parser
 * ============================================================ */

/* Check if a parser feature is enabled */
static inline bool parse_has_feature(mcc_parser_t *p, mcc_feature_id_t feat)
{
    return mcc_ctx_has_feature(p->ctx, feat);
}

/* C99: Mixed declarations and statements */
static inline bool parse_has_mixed_decl(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_MIXED_DECL);
}

/* C99: Declarations in for loop init */
static inline bool parse_has_for_decl(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_FOR_DECL);
}

/* C99: Variable Length Arrays */
static inline bool parse_has_vla(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_VLA);
}

/* C99: Designated initializers */
static inline bool parse_has_designated_init(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_DESIGNATED_INIT);
}

/* C99: Compound literals */
static inline bool parse_has_compound_lit(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_COMPOUND_LIT);
}

/* C99: Flexible array members */
static inline bool parse_has_flexible_array(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_FLEXIBLE_ARRAY);
}

/* C99: inline functions */
static inline bool parse_has_inline(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_INLINE);
}

/* C99: restrict qualifier */
static inline bool parse_has_restrict(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_RESTRICT);
}

/* C99: _Bool type */
static inline bool parse_has_bool(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_BOOL);
}

/* C99: long long type */
static inline bool parse_has_long_long(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_LONG_LONG);
}

/* C11: _Alignas specifier */
static inline bool parse_has_alignas(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_ALIGNAS);
}

/* C11: _Alignof operator */
static inline bool parse_has_alignof(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_ALIGNOF);
}

/* C11: _Static_assert */
static inline bool parse_has_static_assert(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_STATIC_ASSERT);
}

/* C11: _Generic selection */
static inline bool parse_has_generic(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_GENERIC);
}

/* C11: _Noreturn function specifier */
static inline bool parse_has_noreturn(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_NORETURN);
}

/* C11: _Atomic type qualifier */
static inline bool parse_has_atomic(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_ATOMIC);
}

/* C11: _Thread_local storage class */
static inline bool parse_has_thread_local(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_THREAD_LOCAL);
}

/* C11: Anonymous structs/unions */
static inline bool parse_has_anonymous_struct(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_ANONYMOUS_STRUCT);
}

/* C23: nullptr constant */
static inline bool parse_has_nullptr(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_NULLPTR);
}

/* C23: constexpr specifier */
static inline bool parse_has_constexpr(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_CONSTEXPR);
}

/* C23: typeof operator */
static inline bool parse_has_typeof(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_TYPEOF);
}

/* C23: auto type inference */
static inline bool parse_has_auto_type(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_AUTO_TYPE);
}

/* C23: bool keyword (not _Bool) */
static inline bool parse_has_bool_keyword(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_BOOL_KEYWORD);
}

/* C23: true/false keywords */
static inline bool parse_has_true_false(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_TRUE_FALSE);
}

/* GNU: Statement expressions ({ ... }) */
static inline bool parse_has_stmt_expr(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_GNU_STMT_EXPR);
}

/* GNU: Labels as values (&&label) */
static inline bool parse_has_label_addr(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_GNU_LABEL_ADDR);
}

/* GNU: Case ranges (case 1 ... 5:) */
static inline bool parse_has_case_range(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_GNU_CASE_RANGE);
}

/* GNU: __typeof__ */
static inline bool parse_has_gnu_typeof(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_GNU_TYPEOF);
}

/* GNU: __attribute__ */
static inline bool parse_has_gnu_attr(mcc_parser_t *p)
{
    return parse_has_feature(p, MCC_FEAT_GNU_ATTR);
}

/* ============================================================
 * Helper Macros for Feature Warnings
 * ============================================================ */

/* Warn if using a feature not available in current standard */
#define PARSE_WARN_FEATURE(p, feat, loc, msg) \
    do { \
        if (!parse_has_feature(p, feat)) { \
            mcc_warning_at((p)->ctx, loc, msg " is a C99 extension"); \
        } \
    } while (0)

/* Error if using a feature not available in current standard */
#define PARSE_REQUIRE_FEATURE(p, feat, loc, msg) \
    do { \
        if (!parse_has_feature(p, feat)) { \
            mcc_error_at((p)->ctx, loc, msg " requires C99 or later"); \
            return NULL; \
        } \
    } while (0)

#endif /* MCC_PARSE_INTERNAL_H */
