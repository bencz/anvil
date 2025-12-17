/*
 * MCC - Micro C Compiler
 * Semantic Analysis
 */

#ifndef MCC_SEMA_H
#define MCC_SEMA_H

/* Forward declaration */
typedef struct mcc_sema mcc_sema_t;

/* Semantic analyzer state */
struct mcc_sema {
    mcc_context_t *ctx;
    mcc_symtab_t *symtab;
    mcc_type_context_t *types;
    
    /* Current function being analyzed */
    mcc_symbol_t *current_func;
    mcc_type_t *current_return_type;
    
    /* Loop/switch nesting for break/continue */
    int loop_depth;
    int switch_depth;
    
    /* Label tracking */
    mcc_symbol_t **pending_gotos;
    size_t num_pending_gotos;
};

/* Semantic analyzer lifecycle */
mcc_sema_t *mcc_sema_create(mcc_context_t *ctx);
void mcc_sema_destroy(mcc_sema_t *sema);

/* Main analysis entry point */
bool mcc_sema_analyze(mcc_sema_t *sema, mcc_ast_node_t *ast);

/* Declaration analysis */
bool mcc_sema_analyze_decl(mcc_sema_t *sema, mcc_ast_node_t *decl);
bool mcc_sema_analyze_func_decl(mcc_sema_t *sema, mcc_ast_node_t *func);
bool mcc_sema_analyze_var_decl(mcc_sema_t *sema, mcc_ast_node_t *var);

/* Statement analysis */
bool mcc_sema_analyze_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt);
bool mcc_sema_analyze_compound_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt);

/* Expression analysis (returns resolved type) */
mcc_type_t *mcc_sema_analyze_expr(mcc_sema_t *sema, mcc_ast_node_t *expr);

/* Type checking helpers */
bool mcc_sema_check_assignment(mcc_sema_t *sema, mcc_type_t *lhs, mcc_type_t *rhs,
                                mcc_location_t loc);
bool mcc_sema_check_call(mcc_sema_t *sema, mcc_type_t *func_type,
                          mcc_ast_node_t **args, size_t num_args, mcc_location_t loc);
bool mcc_sema_check_return(mcc_sema_t *sema, mcc_type_t *expr_type, mcc_location_t loc);

/* Implicit conversions */
mcc_ast_node_t *mcc_sema_implicit_cast(mcc_sema_t *sema, mcc_ast_node_t *expr,
                                        mcc_type_t *target);

/* Constant expression evaluation */
bool mcc_sema_eval_const_expr(mcc_sema_t *sema, mcc_ast_node_t *expr, int64_t *result);

/* Dump functions */
void mcc_sema_dump(mcc_sema_t *sema, FILE *out);
void mcc_sema_dump_full(mcc_sema_t *sema, mcc_ast_node_t *ast, FILE *out);
void mcc_sema_dump_symtab(mcc_sema_t *sema, FILE *out);
void mcc_sema_dump_globals(mcc_sema_t *sema, FILE *out);
void mcc_sema_dump_verbose(mcc_sema_t *sema, mcc_ast_node_t *ast, FILE *out);

#endif /* MCC_SEMA_H */
