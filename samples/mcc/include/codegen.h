/*
 * MCC - Micro C Compiler
 * Code Generator (ANVIL backend)
 */

#ifndef MCC_CODEGEN_H
#define MCC_CODEGEN_H

#include <anvil/anvil.h>

/* Forward declaration */
typedef struct mcc_codegen mcc_codegen_t;

/* Code generator state */
struct mcc_codegen {
    mcc_context_t *mcc_ctx;
    mcc_symtab_t *symtab;
    mcc_type_context_t *types;
    
    /* ANVIL context and module */
    anvil_ctx_t *anvil_ctx;
    anvil_module_t *anvil_mod;
    
    /* Current function being generated */
    anvil_func_t *current_func;
    anvil_block_t *current_block;
    const char *current_func_name;  /* For __func__ (C99) */
    mcc_type_t *current_return_type;
    anvil_value_t *current_result_pointer;
    
    /* For break/continue */
    anvil_block_t *break_target;
    anvil_block_t *continue_target;
    
    /* Innermost switch being generated.  case/default labels may appear at
     * any depth of its body and resolve to these blocks; the merge block is
     * created by the first edge that reaches it, so it is never orphaned. */
    struct mcc_switch_info {
        mcc_ast_node_t **cases;
        anvil_block_t **blocks;
        size_t num_cases;
        anvil_block_t *default_block;
        anvil_block_t *end_block;
        int id;
        bool active;
    } switch_info;

    /* Entry block of the current function.  It stays unterminated while the
     * body is generated so a declaration that control flow never reaches in
     * sequence still gets storage; codegen_func branches it to the body. */
    anvil_block_t *alloca_block;

    /* Runtime lengths of the variable-length array dimensions evaluated in
     * the current function (C11 6.7.6.2p5: once, at the declaration). */
    struct {
        mcc_type_t *type;
        anvil_value_t *length;
    } *vla_lengths;
    size_t num_vla_lengths;
    size_t cap_vla_lengths;
    
    /* Local variable mapping (name -> anvil_value_t*) */
    struct {
        const char *name;
        anvil_value_t *value;
    } *locals;
    size_t num_locals;
    size_t cap_locals;
    
    /* String literal pool */
    struct {
        const char *str;
        anvil_value_t *value;
    } *strings;
    size_t num_strings;
    size_t cap_strings;
    
    /* Label mapping for goto */
    struct {
        const char *name;
        anvil_block_t *block;
    } *labels;
    size_t num_labels;
    size_t cap_labels;
    
    /* Function mapping (symbol -> anvil_func_t*) */
    struct {
        mcc_symbol_t *sym;
        anvil_func_t *func;
    } *funcs;
    size_t num_funcs;
    size_t cap_funcs;
    
    /* Label counter for unique block names */
    int label_counter;
    
    /* Optimization level */
    mcc_opt_level_t opt_level;
    
    /* Global variable cache (name -> anvil_value_t*) */
    struct {
        const char *name;
        anvil_value_t *value;
        bool is_defined;
    } *globals;
    size_t num_globals;
    size_t cap_globals;
};

/* Code generator lifecycle */
mcc_codegen_t *mcc_codegen_create(mcc_context_t *ctx, mcc_symtab_t *symtab,
                                   mcc_type_context_t *types);
void mcc_codegen_destroy(mcc_codegen_t *cg);

/* Configuration */
bool mcc_codegen_set_target(mcc_codegen_t *cg, mcc_arch_t arch);
void mcc_codegen_set_opt_level(mcc_codegen_t *cg, mcc_opt_level_t level);

/* Main code generation entry point */
bool mcc_codegen_generate(mcc_codegen_t *cg, mcc_ast_node_t *ast);

/* Add AST from another file to the same module (multi-file support) */
bool mcc_codegen_add_ast(mcc_codegen_t *cg, mcc_ast_node_t *ast);

/* Finalize code generation after all files have been added */
bool mcc_codegen_finalize(mcc_codegen_t *cg);

/* Get generated code */
char *mcc_codegen_get_output(mcc_codegen_t *cg, size_t *len);

/* Declaration code generation */
void mcc_codegen_decl(mcc_codegen_t *cg, mcc_ast_node_t *decl);
void mcc_codegen_func(mcc_codegen_t *cg, mcc_ast_node_t *func);
void mcc_codegen_global_var(mcc_codegen_t *cg, mcc_ast_node_t *var);

/* Statement code generation */
void mcc_codegen_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);
void mcc_codegen_compound_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);
void mcc_codegen_if_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);
void mcc_codegen_while_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);
void mcc_codegen_do_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);
void mcc_codegen_for_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);
void mcc_codegen_switch_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);
void mcc_codegen_return_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);

/* Expression code generation (returns ANVIL value) */
anvil_value_t *mcc_codegen_expr(mcc_codegen_t *cg, mcc_ast_node_t *expr);
anvil_value_t *mcc_codegen_lvalue(mcc_codegen_t *cg, mcc_ast_node_t *expr);

/* Type conversion */
anvil_type_t *mcc_codegen_type(mcc_codegen_t *cg, mcc_type_t *type);

/* Helpers */
anvil_value_t *mcc_codegen_load(mcc_codegen_t *cg, anvil_value_t *ptr, mcc_type_t *type);
void mcc_codegen_store(mcc_codegen_t *cg, anvil_value_t *val, anvil_value_t *ptr);

#endif /* MCC_CODEGEN_H */
