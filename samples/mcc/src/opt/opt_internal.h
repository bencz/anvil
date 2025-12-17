/*
 * MCC - Micro C Compiler
 * AST Optimization Internal Header
 * 
 * Internal definitions for the optimization passes.
 */

#ifndef MCC_OPT_INTERNAL_H
#define MCC_OPT_INTERNAL_H

#include "mcc.h"
#include "ast_opt.h"

/* ============================================================
 * Pass Function Type
 * 
 * Each optimization pass is implemented as a function that:
 * - Takes the optimizer context and an AST node
 * - Returns the number of changes made (0 if no changes)
 * - May modify the AST in place or replace nodes
 * ============================================================ */

typedef int (*mcc_opt_pass_fn)(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);

/* ============================================================
 * Pass Registration Structure
 * ============================================================ */

typedef struct mcc_opt_pass_entry {
    mcc_opt_pass_id_t id;
    const char *name;
    const char *description;
    int min_opt_level;
    bool modifies_ast;
    bool requires_sema;
    mcc_opt_pass_fn fn;
} mcc_opt_pass_entry_t;

/* ============================================================
 * Pass Implementation Functions
 * 
 * Each pass is implemented in its own file and registered here.
 * ============================================================ */

/* O0 Passes - Always-on normalization */
int opt_pass_normalize(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_trivial_const(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_identity_ops(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_double_neg(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);

/* Og Passes - Debug-friendly */
int opt_pass_copy_prop(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_store_load_prop(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_unreachable_after_return(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);

/* O1 Passes - Basic optimizations */
int opt_pass_const_fold(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_const_prop(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_dce(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_dead_store(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_strength_red(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_algebraic(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_branch_simp(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);

/* O2 Passes - Standard optimizations */
int opt_pass_cse(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_licm(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_loop_simp(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_tail_call(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_inline_small(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);

/* O3 Passes - Aggressive optimizations */
int opt_pass_loop_unroll(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_inline_aggr(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
int opt_pass_vectorize(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);

/* ============================================================
 * Helper Functions
 * ============================================================ */

/* Check if an expression is a compile-time constant */
bool opt_is_const_expr(mcc_ast_node_t *expr);

/* Evaluate a constant expression (returns true if successful) */
bool opt_eval_const_int(mcc_ast_node_t *expr, int64_t *result);
bool opt_eval_const_float(mcc_ast_node_t *expr, double *result);

/* Create a new integer literal node */
mcc_ast_node_t *opt_make_int_lit(mcc_ast_opt_t *opt, int64_t value, 
                                  mcc_location_t loc);

/* Create a new float literal node */
mcc_ast_node_t *opt_make_float_lit(mcc_ast_opt_t *opt, double value,
                                    mcc_location_t loc);

/* Check if expression has side effects */
bool opt_has_side_effects(mcc_ast_node_t *expr);

/* Check if expression is pure (no side effects, same result each time) */
bool opt_is_pure_expr(mcc_ast_node_t *expr);

/* Check if two expressions are equivalent */
bool opt_exprs_equal(mcc_ast_node_t *a, mcc_ast_node_t *b);

/* Deep copy an AST node */
mcc_ast_node_t *opt_copy_node(mcc_ast_opt_t *opt, mcc_ast_node_t *node);

/* Replace a node in the AST (updates parent references) */
void opt_replace_node(mcc_ast_node_t *old_node, mcc_ast_node_t *new_node);

/* AST traversal helpers */
typedef bool (*opt_visit_fn)(mcc_ast_opt_t *opt, mcc_ast_node_t *node, void *data);

/* Visit all nodes in pre-order (parent before children) */
void opt_visit_preorder(mcc_ast_opt_t *opt, mcc_ast_node_t *ast, 
                        opt_visit_fn fn, void *data);

/* Visit all nodes in post-order (children before parent) */
void opt_visit_postorder(mcc_ast_opt_t *opt, mcc_ast_node_t *ast,
                         opt_visit_fn fn, void *data);

/* Visit all expressions in an AST */
void opt_visit_exprs(mcc_ast_opt_t *opt, mcc_ast_node_t *ast,
                     opt_visit_fn fn, void *data);

/* Visit all statements in an AST */
void opt_visit_stmts(mcc_ast_opt_t *opt, mcc_ast_node_t *ast,
                     opt_visit_fn fn, void *data);

#endif /* MCC_OPT_INTERNAL_H */
