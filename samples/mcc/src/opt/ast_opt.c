/*
 * MCC - Micro C Compiler
 * AST Optimization Pass Manager
 * 
 * Main module for AST-level optimizations.
 */

#include "opt_internal.h"

/* ============================================================
 * Pass Information Table
 * Similar to c_std_info_table in c_std.c
 * ============================================================ */

static const mcc_opt_pass_info_t pass_info_table[] = {
    /* O0 Passes - Always-on */
    {
        .id = MCC_OPT_PASS_NORMALIZE,
        .name = "normalize",
        .description = "AST normalization to canonical form",
        .min_opt_level = 0,
        .modifies_ast = true,
        .requires_sema = false
    },
    {
        .id = MCC_OPT_PASS_TRIVIAL_CONST,
        .name = "trivial_const",
        .description = "Trivial constant simplification (1*x -> x, 0+x -> x)",
        .min_opt_level = 0,
        .modifies_ast = true,
        .requires_sema = false
    },
    {
        .id = MCC_OPT_PASS_IDENTITY_OPS,
        .name = "identity_ops",
        .description = "Identity operation removal (x+0, x*1, x|0, x&~0)",
        .min_opt_level = 0,
        .modifies_ast = true,
        .requires_sema = false
    },
    {
        .id = MCC_OPT_PASS_DOUBLE_NEG,
        .name = "double_neg",
        .description = "Double negation removal (--x, !!x)",
        .min_opt_level = 0,
        .modifies_ast = true,
        .requires_sema = false
    },
    
    /* Og Passes - Debug-friendly */
    {
        .id = MCC_OPT_PASS_COPY_PROP,
        .name = "copy_prop",
        .description = "Copy propagation",
        .min_opt_level = 1, /* Og maps to level 1 internally */
        .modifies_ast = true,
        .requires_sema = true
    },
    {
        .id = MCC_OPT_PASS_STORE_LOAD_PROP,
        .name = "store_load_prop",
        .description = "Store-load propagation",
        .min_opt_level = 1,
        .modifies_ast = true,
        .requires_sema = true
    },
    {
        .id = MCC_OPT_PASS_UNREACHABLE_AFTER_RETURN,
        .name = "unreachable_after_return",
        .description = "Remove unreachable code after return",
        .min_opt_level = 1,
        .modifies_ast = true,
        .requires_sema = false
    },
    
    /* O1 Passes - Basic optimizations */
    {
        .id = MCC_OPT_PASS_CONST_FOLD,
        .name = "const_fold",
        .description = "Constant folding (3+5 -> 8)",
        .min_opt_level = 2,
        .modifies_ast = true,
        .requires_sema = false
    },
    {
        .id = MCC_OPT_PASS_CONST_PROP,
        .name = "const_prop",
        .description = "Constant propagation",
        .min_opt_level = 2,
        .modifies_ast = true,
        .requires_sema = true
    },
    {
        .id = MCC_OPT_PASS_DCE,
        .name = "dce",
        .description = "Dead code elimination",
        .min_opt_level = 2,
        .modifies_ast = true,
        .requires_sema = true
    },
    {
        .id = MCC_OPT_PASS_DEAD_STORE,
        .name = "dead_store",
        .description = "Dead store elimination",
        .min_opt_level = 2,
        .modifies_ast = true,
        .requires_sema = true
    },
    {
        .id = MCC_OPT_PASS_STRENGTH_RED,
        .name = "strength_red",
        .description = "Strength reduction (x*2 -> x<<1)",
        .min_opt_level = 2,
        .modifies_ast = true,
        .requires_sema = true
    },
    {
        .id = MCC_OPT_PASS_ALGEBRAIC,
        .name = "algebraic",
        .description = "Algebraic simplifications",
        .min_opt_level = 2,
        .modifies_ast = true,
        .requires_sema = false
    },
    {
        .id = MCC_OPT_PASS_BRANCH_SIMP,
        .name = "branch_simp",
        .description = "Branch simplification",
        .min_opt_level = 2,
        .modifies_ast = true,
        .requires_sema = false
    },
    
    /* O2 Passes - Standard optimizations */
    {
        .id = MCC_OPT_PASS_CSE,
        .name = "cse",
        .description = "Common subexpression elimination",
        .min_opt_level = 3,
        .modifies_ast = true,
        .requires_sema = true
    },
    {
        .id = MCC_OPT_PASS_LICM,
        .name = "licm",
        .description = "Loop-invariant code motion",
        .min_opt_level = 3,
        .modifies_ast = true,
        .requires_sema = true
    },
    {
        .id = MCC_OPT_PASS_LOOP_SIMP,
        .name = "loop_simp",
        .description = "Loop simplification",
        .min_opt_level = 3,
        .modifies_ast = true,
        .requires_sema = true
    },
    {
        .id = MCC_OPT_PASS_TAIL_CALL,
        .name = "tail_call",
        .description = "Tail call optimization",
        .min_opt_level = 3,
        .modifies_ast = true,
        .requires_sema = true
    },
    {
        .id = MCC_OPT_PASS_INLINE_SMALL,
        .name = "inline_small",
        .description = "Inline small functions",
        .min_opt_level = 3,
        .modifies_ast = true,
        .requires_sema = true
    },
    
    /* O3 Passes - Aggressive optimizations */
    {
        .id = MCC_OPT_PASS_LOOP_UNROLL,
        .name = "loop_unroll",
        .description = "Loop unrolling",
        .min_opt_level = 4,
        .modifies_ast = true,
        .requires_sema = true
    },
    {
        .id = MCC_OPT_PASS_INLINE_AGGR,
        .name = "inline_aggr",
        .description = "Aggressive function inlining",
        .min_opt_level = 4,
        .modifies_ast = true,
        .requires_sema = true
    },
    {
        .id = MCC_OPT_PASS_VECTORIZE,
        .name = "vectorize",
        .description = "Vectorization hints",
        .min_opt_level = 4,
        .modifies_ast = true,
        .requires_sema = true
    },
    
    /* Sentinel */
    {
        .id = MCC_OPT_PASS_COUNT,
        .name = NULL,
        .description = NULL,
        .min_opt_level = 0,
        .modifies_ast = false,
        .requires_sema = false
    }
};

/* ============================================================
 * Pass Function Table
 * Maps pass IDs to implementation functions
 * ============================================================ */

static const mcc_opt_pass_entry_t pass_entries[] = {
    /* O0 Passes */
    { MCC_OPT_PASS_NORMALIZE, "normalize", "AST normalization", 0, true, false, opt_pass_normalize },
    { MCC_OPT_PASS_TRIVIAL_CONST, "trivial_const", "Trivial constants", 0, true, false, opt_pass_trivial_const },
    { MCC_OPT_PASS_IDENTITY_OPS, "identity_ops", "Identity operations", 0, true, false, opt_pass_identity_ops },
    { MCC_OPT_PASS_DOUBLE_NEG, "double_neg", "Double negation", 0, true, false, opt_pass_double_neg },
    
    /* Og Passes */
    { MCC_OPT_PASS_COPY_PROP, "copy_prop", "Copy propagation", 1, true, true, opt_pass_copy_prop },
    { MCC_OPT_PASS_STORE_LOAD_PROP, "store_load_prop", "Store-load propagation", 1, true, true, opt_pass_store_load_prop },
    { MCC_OPT_PASS_UNREACHABLE_AFTER_RETURN, "unreachable_after_return", "Unreachable code", 1, true, false, opt_pass_unreachable_after_return },
    
    /* O1 Passes */
    { MCC_OPT_PASS_CONST_FOLD, "const_fold", "Constant folding", 2, true, false, opt_pass_const_fold },
    { MCC_OPT_PASS_CONST_PROP, "const_prop", "Constant propagation", 2, true, true, opt_pass_const_prop },
    { MCC_OPT_PASS_DCE, "dce", "Dead code elimination", 2, true, true, opt_pass_dce },
    { MCC_OPT_PASS_DEAD_STORE, "dead_store", "Dead store elimination", 2, true, true, opt_pass_dead_store },
    { MCC_OPT_PASS_STRENGTH_RED, "strength_red", "Strength reduction", 2, true, true, opt_pass_strength_red },
    { MCC_OPT_PASS_ALGEBRAIC, "algebraic", "Algebraic simplification", 2, true, false, opt_pass_algebraic },
    { MCC_OPT_PASS_BRANCH_SIMP, "branch_simp", "Branch simplification", 2, true, false, opt_pass_branch_simp },
    
    /* O2 Passes */
    { MCC_OPT_PASS_CSE, "cse", "Common subexpression elimination", 3, true, true, opt_pass_cse },
    { MCC_OPT_PASS_LICM, "licm", "Loop-invariant code motion", 3, true, true, opt_pass_licm },
    { MCC_OPT_PASS_LOOP_SIMP, "loop_simp", "Loop simplification", 3, true, true, opt_pass_loop_simp },
    { MCC_OPT_PASS_TAIL_CALL, "tail_call", "Tail call optimization", 3, true, true, opt_pass_tail_call },
    { MCC_OPT_PASS_INLINE_SMALL, "inline_small", "Small function inlining", 3, true, true, opt_pass_inline_small },
    
    /* O3 Passes */
    { MCC_OPT_PASS_LOOP_UNROLL, "loop_unroll", "Loop unrolling", 4, true, true, opt_pass_loop_unroll },
    { MCC_OPT_PASS_INLINE_AGGR, "inline_aggr", "Aggressive inlining", 4, true, true, opt_pass_inline_aggr },
    { MCC_OPT_PASS_VECTORIZE, "vectorize", "Vectorization", 4, true, true, opt_pass_vectorize },
    
    /* Sentinel */
    { MCC_OPT_PASS_COUNT, NULL, NULL, 0, false, false, NULL }
};

/* ============================================================
 * Pass Initialization by Optimization Level
 * ============================================================ */

static void init_o0_passes(mcc_opt_passes_t *passes)
{
    MCC_OPT_PASSES_INIT(*passes);
    
    /* O0: Only normalization passes that don't change semantics */
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_NORMALIZE);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_TRIVIAL_CONST);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_IDENTITY_OPS);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_DOUBLE_NEG);
}

static void init_og_passes(mcc_opt_passes_t *passes)
{
    /* Start with O0 passes */
    init_o0_passes(passes);
    
    /* Add debug-friendly passes */
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_COPY_PROP);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_STORE_LOAD_PROP);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_UNREACHABLE_AFTER_RETURN);
}

static void init_o1_passes(mcc_opt_passes_t *passes)
{
    /* Start with Og passes */
    init_og_passes(passes);
    
    /* Add basic optimizations */
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_CONST_FOLD);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_CONST_PROP);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_DCE);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_DEAD_STORE);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_STRENGTH_RED);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_ALGEBRAIC);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_BRANCH_SIMP);
}

static void init_o2_passes(mcc_opt_passes_t *passes)
{
    /* Start with O1 passes */
    init_o1_passes(passes);
    
    /* Add standard optimizations */
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_CSE);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_LICM);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_LOOP_SIMP);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_TAIL_CALL);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_INLINE_SMALL);
}

static void init_o3_passes(mcc_opt_passes_t *passes)
{
    /* Start with O2 passes */
    init_o2_passes(passes);
    
    /* Add aggressive optimizations */
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_LOOP_UNROLL);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_INLINE_AGGR);
    MCC_OPT_PASSES_SET(*passes, MCC_OPT_PASS_VECTORIZE);
}

/* ============================================================
 * Public API Implementation
 * ============================================================ */

void mcc_ast_opt_init_passes(int opt_level, mcc_opt_passes_t *passes)
{
    if (!passes) return;
    
    switch (opt_level) {
        case 0: /* MCC_OPT_NONE */
            init_o0_passes(passes);
            break;
        case 1: /* MCC_OPT_DEBUG (Og) */
            init_og_passes(passes);
            break;
        case 2: /* MCC_OPT_BASIC (O1) */
            init_o1_passes(passes);
            break;
        case 3: /* MCC_OPT_STANDARD (O2) */
            init_o2_passes(passes);
            break;
        case 4: /* MCC_OPT_AGGRESSIVE (O3) */
            init_o3_passes(passes);
            break;
        default:
            init_o0_passes(passes);
            break;
    }
}

mcc_ast_opt_t *mcc_ast_opt_create(mcc_context_t *ctx)
{
    mcc_ast_opt_t *opt = mcc_alloc(ctx, sizeof(mcc_ast_opt_t));
    if (!opt) return NULL;
    
    memset(opt, 0, sizeof(mcc_ast_opt_t));
    opt->ctx = ctx;
    opt->opt_level = 0;
    
    /* Initialize with O0 passes by default */
    mcc_ast_opt_init_passes(0, &opt->enabled_passes);
    MCC_OPT_PASSES_INIT(opt->disabled_passes);
    
    return opt;
}

void mcc_ast_opt_destroy(mcc_ast_opt_t *opt)
{
    /* Memory is arena-allocated, nothing to free */
    (void)opt;
}

void mcc_ast_opt_set_level(mcc_ast_opt_t *opt, int level)
{
    if (!opt) return;
    
    opt->opt_level = level;
    mcc_ast_opt_init_passes(level, &opt->enabled_passes);
}

void mcc_ast_opt_set_sema(mcc_ast_opt_t *opt, mcc_sema_t *sema)
{
    if (!opt) return;
    opt->sema = sema;
}

void mcc_ast_opt_enable_pass(mcc_ast_opt_t *opt, mcc_opt_pass_id_t pass)
{
    if (!opt || pass >= MCC_OPT_PASS_COUNT) return;
    
    MCC_OPT_PASSES_SET(opt->enabled_passes, pass);
    MCC_OPT_PASSES_CLEAR(opt->disabled_passes, pass);
}

void mcc_ast_opt_disable_pass(mcc_ast_opt_t *opt, mcc_opt_pass_id_t pass)
{
    if (!opt || pass >= MCC_OPT_PASS_COUNT) return;
    
    MCC_OPT_PASSES_CLEAR(opt->enabled_passes, pass);
    MCC_OPT_PASSES_SET(opt->disabled_passes, pass);
}

void mcc_ast_opt_set_verbose(mcc_ast_opt_t *opt, bool verbose)
{
    if (!opt) return;
    opt->verbose = verbose;
}

bool mcc_ast_opt_pass_enabled(mcc_ast_opt_t *opt, mcc_opt_pass_id_t pass)
{
    if (!opt || pass >= MCC_OPT_PASS_COUNT) return false;
    
    /* Check if explicitly disabled */
    if (MCC_OPT_PASSES_HAS(opt->disabled_passes, pass)) {
        return false;
    }
    
    return MCC_OPT_PASSES_HAS(opt->enabled_passes, pass);
}

const mcc_opt_pass_info_t *mcc_ast_opt_get_pass_info(mcc_opt_pass_id_t pass)
{
    if (pass >= MCC_OPT_PASS_COUNT) return NULL;
    
    for (size_t i = 0; pass_info_table[i].name != NULL; i++) {
        if (pass_info_table[i].id == pass) {
            return &pass_info_table[i];
        }
    }
    
    return NULL;
}

const char *mcc_ast_opt_pass_name(mcc_opt_pass_id_t pass)
{
    const mcc_opt_pass_info_t *info = mcc_ast_opt_get_pass_info(pass);
    return info ? info->name : "unknown";
}

/* Find pass entry by ID */
static const mcc_opt_pass_entry_t *find_pass_entry(mcc_opt_pass_id_t pass)
{
    for (size_t i = 0; pass_entries[i].name != NULL; i++) {
        if (pass_entries[i].id == pass) {
            return &pass_entries[i];
        }
    }
    return NULL;
}

int mcc_ast_opt_run_pass(mcc_ast_opt_t *opt, mcc_ast_node_t *ast, 
                          mcc_opt_pass_id_t pass)
{
    if (!opt || !ast || pass >= MCC_OPT_PASS_COUNT) return 0;
    
    const mcc_opt_pass_entry_t *entry = find_pass_entry(pass);
    if (!entry || !entry->fn) return 0;
    
    /* Check if pass requires sema and we have it */
    if (entry->requires_sema && !opt->sema) {
        if (opt->verbose) {
            fprintf(stderr, "  [skip] %s: requires semantic info\n", entry->name);
        }
        return 0;
    }
    
    if (opt->verbose) {
        fprintf(stderr, "  [run] %s\n", entry->name);
    }
    
    int changes = entry->fn(opt, ast);
    
    if (changes > 0) {
        opt->total_changes += changes;
        opt->pass_changes[pass] += changes;
        
        if (opt->verbose) {
            fprintf(stderr, "    -> %d change(s)\n", changes);
        }
    }
    
    return changes;
}

bool mcc_ast_opt_run(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    if (!opt || !ast) return false;
    
    if (opt->verbose) {
        fprintf(stderr, "AST Optimization (level %d):\n", opt->opt_level);
    }
    
    opt->total_changes = 0;
    opt->iterations = 0;
    memset(opt->pass_changes, 0, sizeof(opt->pass_changes));
    
    /* Run passes in order, iterating until no more changes */
    const int max_iterations = 10;
    int changes;
    
    do {
        changes = 0;
        opt->iterations++;
        
        if (opt->verbose) {
            fprintf(stderr, "Iteration %d:\n", opt->iterations);
        }
        
        /* Run each enabled pass */
        for (int pass = 0; pass < MCC_OPT_PASS_COUNT; pass++) {
            if (mcc_ast_opt_pass_enabled(opt, pass)) {
                changes += mcc_ast_opt_run_pass(opt, ast, pass);
            }
        }
        
    } while (changes > 0 && opt->iterations < max_iterations);
    
    if (opt->verbose) {
        fprintf(stderr, "Optimization complete: %d total changes in %d iterations\n",
                opt->total_changes, opt->iterations);
    }
    
    return true;
}

int mcc_ast_opt_get_total_changes(mcc_ast_opt_t *opt)
{
    return opt ? opt->total_changes : 0;
}

int mcc_ast_opt_get_pass_changes(mcc_ast_opt_t *opt, mcc_opt_pass_id_t pass)
{
    if (!opt || pass >= MCC_OPT_PASS_COUNT) return 0;
    return opt->pass_changes[pass];
}
