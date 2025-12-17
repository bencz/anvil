/*
 * MCC - Micro C Compiler
 * AST Optimization System
 * 
 * This file defines the AST-level optimization passes and pass manager,
 * following the modular pattern of c_std.h for feature management.
 */

#ifndef MCC_AST_OPT_H
#define MCC_AST_OPT_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Forward declarations */
struct mcc_context;
struct mcc_ast_node;
struct mcc_sema;

/* ============================================================
 * Optimization Pass System
 * 
 * Uses a scalable feature flag system similar to c_std.h
 * Currently sized for 64 passes (1 x 64-bit word).
 * Can be expanded by increasing MCC_OPT_PASS_WORDS.
 * ============================================================ */

#define MCC_OPT_PASS_WORDS 1     /* 1 * 64 = 64 passes */
#define MCC_OPT_PASS_BITS  64    /* Bits per word */

/*
 * Pass set structure - array of 64-bit words
 */
typedef struct mcc_opt_passes {
    uint64_t bits[MCC_OPT_PASS_WORDS];
} mcc_opt_passes_t;

/*
 * Optimization Pass IDs
 * 
 * Passes are organized by category and optimization level:
 * - O0 (0-7):   Always-on passes (normalization, trivial simplifications)
 * - Og (8-15):  Debug-friendly passes (minimal changes)
 * - O1 (16-31): Basic optimizations
 * - O2 (32-47): Standard optimizations
 * - O3 (48-63): Aggressive optimizations
 */
typedef enum {
    /* ============================================================
     * O0 Passes (0-7): Always-on, semantic-preserving normalization
     * These run even at -O0 to ensure consistent AST structure
     * ============================================================ */
    
    MCC_OPT_PASS_NORMALIZE = 0,     /* AST normalization (canonical form) */
    MCC_OPT_PASS_TRIVIAL_CONST,     /* Trivial constant simplification (1*x -> x) */
    MCC_OPT_PASS_IDENTITY_OPS,      /* Identity operations (x+0, x*1, x|0, x&~0) */
    MCC_OPT_PASS_DOUBLE_NEG,        /* Double negation removal (--x, !!x for bool) */
    MCC_OPT_PASS_RESERVED_4,
    MCC_OPT_PASS_RESERVED_5,
    MCC_OPT_PASS_RESERVED_6,
    MCC_OPT_PASS_RESERVED_7,
    
    /* ============================================================
     * Og Passes (8-15): Debug-friendly optimizations
     * Minimal changes that don't affect debugging
     * ============================================================ */
    
    MCC_OPT_PASS_COPY_PROP = 8,     /* Copy propagation */
    MCC_OPT_PASS_STORE_LOAD_PROP,   /* Store-load propagation */
    MCC_OPT_PASS_UNREACHABLE_AFTER_RETURN, /* Remove code after return */
    MCC_OPT_PASS_RESERVED_11,
    MCC_OPT_PASS_RESERVED_12,
    MCC_OPT_PASS_RESERVED_13,
    MCC_OPT_PASS_RESERVED_14,
    MCC_OPT_PASS_RESERVED_15,
    
    /* ============================================================
     * O1 Passes (16-31): Basic optimizations
     * ============================================================ */
    
    MCC_OPT_PASS_CONST_FOLD = 16,   /* Constant folding (3+5 -> 8) */
    MCC_OPT_PASS_CONST_PROP,        /* Constant propagation */
    MCC_OPT_PASS_DCE,               /* Dead code elimination */
    MCC_OPT_PASS_DEAD_STORE,        /* Dead store elimination */
    MCC_OPT_PASS_STRENGTH_RED,      /* Strength reduction (x*2 -> x<<1) */
    MCC_OPT_PASS_ALGEBRAIC,         /* Algebraic simplifications */
    MCC_OPT_PASS_BRANCH_SIMP,       /* Branch simplification */
    MCC_OPT_PASS_RESERVED_23,
    MCC_OPT_PASS_RESERVED_24,
    MCC_OPT_PASS_RESERVED_25,
    MCC_OPT_PASS_RESERVED_26,
    MCC_OPT_PASS_RESERVED_27,
    MCC_OPT_PASS_RESERVED_28,
    MCC_OPT_PASS_RESERVED_29,
    MCC_OPT_PASS_RESERVED_30,
    MCC_OPT_PASS_RESERVED_31,
    
    /* ============================================================
     * O2 Passes (32-47): Standard optimizations
     * ============================================================ */
    
    MCC_OPT_PASS_CSE = 32,          /* Common subexpression elimination */
    MCC_OPT_PASS_LICM,              /* Loop-invariant code motion */
    MCC_OPT_PASS_LOOP_SIMP,         /* Loop simplification */
    MCC_OPT_PASS_TAIL_CALL,         /* Tail call optimization */
    MCC_OPT_PASS_INLINE_SMALL,      /* Inline small functions */
    MCC_OPT_PASS_RESERVED_37,
    MCC_OPT_PASS_RESERVED_38,
    MCC_OPT_PASS_RESERVED_39,
    MCC_OPT_PASS_RESERVED_40,
    MCC_OPT_PASS_RESERVED_41,
    MCC_OPT_PASS_RESERVED_42,
    MCC_OPT_PASS_RESERVED_43,
    MCC_OPT_PASS_RESERVED_44,
    MCC_OPT_PASS_RESERVED_45,
    MCC_OPT_PASS_RESERVED_46,
    MCC_OPT_PASS_RESERVED_47,
    
    /* ============================================================
     * O3 Passes (48-63): Aggressive optimizations
     * ============================================================ */
    
    MCC_OPT_PASS_LOOP_UNROLL = 48,  /* Loop unrolling */
    MCC_OPT_PASS_INLINE_AGGR,       /* Aggressive inlining */
    MCC_OPT_PASS_VECTORIZE,         /* Vectorization hints */
    MCC_OPT_PASS_RESERVED_51,
    MCC_OPT_PASS_RESERVED_52,
    MCC_OPT_PASS_RESERVED_53,
    MCC_OPT_PASS_RESERVED_54,
    MCC_OPT_PASS_RESERVED_55,
    MCC_OPT_PASS_RESERVED_56,
    MCC_OPT_PASS_RESERVED_57,
    MCC_OPT_PASS_RESERVED_58,
    MCC_OPT_PASS_RESERVED_59,
    MCC_OPT_PASS_RESERVED_60,
    MCC_OPT_PASS_RESERVED_61,
    MCC_OPT_PASS_RESERVED_62,
    MCC_OPT_PASS_RESERVED_63,
    
    /* Total count */
    MCC_OPT_PASS_COUNT = 64
} mcc_opt_pass_id_t;

/* ============================================================
 * Pass Manipulation Macros
 * ============================================================ */

/* Get word index and bit position for a pass */
#define MCC_OPT_PASS_WORD(pass)  ((pass) / MCC_OPT_PASS_BITS)
#define MCC_OPT_PASS_BIT(pass)   ((pass) % MCC_OPT_PASS_BITS)
#define MCC_OPT_PASS_MASK(pass)  (1ULL << MCC_OPT_PASS_BIT(pass))

/* Initialize passes to zero */
#define MCC_OPT_PASSES_INIT(p) memset(&(p), 0, sizeof(mcc_opt_passes_t))

/* Set a single pass */
#define MCC_OPT_PASSES_SET(p, pass) \
    ((p).bits[MCC_OPT_PASS_WORD(pass)] |= MCC_OPT_PASS_MASK(pass))

/* Clear a single pass */
#define MCC_OPT_PASSES_CLEAR(p, pass) \
    ((p).bits[MCC_OPT_PASS_WORD(pass)] &= ~MCC_OPT_PASS_MASK(pass))

/* Test if a pass is enabled */
#define MCC_OPT_PASSES_HAS(p, pass) \
    (((p).bits[MCC_OPT_PASS_WORD(pass)] & MCC_OPT_PASS_MASK(pass)) != 0)

/* Copy passes */
#define MCC_OPT_PASSES_COPY(dst, src) memcpy(&(dst), &(src), sizeof(mcc_opt_passes_t))

/* ============================================================
 * Pass Set Operations
 * ============================================================ */

/* Combine two pass sets (OR) */
static inline void mcc_opt_passes_or(mcc_opt_passes_t *dst, const mcc_opt_passes_t *src)
{
    for (int i = 0; i < MCC_OPT_PASS_WORDS; i++) {
        dst->bits[i] |= src->bits[i];
    }
}

/* Intersect two pass sets (AND) */
static inline void mcc_opt_passes_and(mcc_opt_passes_t *dst, const mcc_opt_passes_t *src)
{
    for (int i = 0; i < MCC_OPT_PASS_WORDS; i++) {
        dst->bits[i] &= src->bits[i];
    }
}

/* Remove passes (AND NOT) */
static inline void mcc_opt_passes_remove(mcc_opt_passes_t *dst, const mcc_opt_passes_t *src)
{
    for (int i = 0; i < MCC_OPT_PASS_WORDS; i++) {
        dst->bits[i] &= ~src->bits[i];
    }
}

/* Check if pass set is empty */
static inline bool mcc_opt_passes_is_empty(const mcc_opt_passes_t *passes)
{
    for (int i = 0; i < MCC_OPT_PASS_WORDS; i++) {
        if (passes->bits[i] != 0) {
            return false;
        }
    }
    return true;
}

/* ============================================================
 * Pass Information Structure
 * ============================================================ */

typedef struct mcc_opt_pass_info {
    mcc_opt_pass_id_t id;           /* Pass ID */
    const char *name;               /* Short name (e.g., "const_fold") */
    const char *description;        /* Full description */
    int min_opt_level;              /* Minimum optimization level (0-4) */
    bool modifies_ast;              /* Does this pass modify the AST? */
    bool requires_sema;             /* Does this pass require semantic info? */
} mcc_opt_pass_info_t;

/* ============================================================
 * AST Optimizer Structure
 * ============================================================ */

typedef struct mcc_ast_opt {
    struct mcc_context *ctx;        /* Compiler context */
    struct mcc_sema *sema;          /* Semantic analyzer (for type info) */
    
    /* Configuration */
    int opt_level;                  /* Optimization level (0-4) */
    mcc_opt_passes_t enabled_passes; /* Enabled passes */
    mcc_opt_passes_t disabled_passes; /* Explicitly disabled passes */
    
    /* Statistics */
    int total_changes;              /* Total AST modifications */
    int pass_changes[MCC_OPT_PASS_COUNT]; /* Changes per pass */
    int iterations;                 /* Number of optimization iterations */
    
    /* Debug */
    bool verbose;                   /* Print optimization info */
    bool dump_after_pass;           /* Dump AST after each pass */
} mcc_ast_opt_t;

/* ============================================================
 * Public API
 * ============================================================ */

/* Create and destroy optimizer */
mcc_ast_opt_t *mcc_ast_opt_create(struct mcc_context *ctx);
void mcc_ast_opt_destroy(mcc_ast_opt_t *opt);

/* Configuration */
void mcc_ast_opt_set_level(mcc_ast_opt_t *opt, int level);
void mcc_ast_opt_set_sema(mcc_ast_opt_t *opt, struct mcc_sema *sema);
void mcc_ast_opt_enable_pass(mcc_ast_opt_t *opt, mcc_opt_pass_id_t pass);
void mcc_ast_opt_disable_pass(mcc_ast_opt_t *opt, mcc_opt_pass_id_t pass);
void mcc_ast_opt_set_verbose(mcc_ast_opt_t *opt, bool verbose);

/* Run optimization */
bool mcc_ast_opt_run(mcc_ast_opt_t *opt, struct mcc_ast_node *ast);

/* Run a single pass */
int mcc_ast_opt_run_pass(mcc_ast_opt_t *opt, struct mcc_ast_node *ast, 
                          mcc_opt_pass_id_t pass);

/* Query */
bool mcc_ast_opt_pass_enabled(mcc_ast_opt_t *opt, mcc_opt_pass_id_t pass);
const mcc_opt_pass_info_t *mcc_ast_opt_get_pass_info(mcc_opt_pass_id_t pass);
const char *mcc_ast_opt_pass_name(mcc_opt_pass_id_t pass);

/* Initialize passes for an optimization level */
void mcc_ast_opt_init_passes(int opt_level, mcc_opt_passes_t *passes);

/* Statistics */
int mcc_ast_opt_get_total_changes(mcc_ast_opt_t *opt);
int mcc_ast_opt_get_pass_changes(mcc_ast_opt_t *opt, mcc_opt_pass_id_t pass);

#endif /* MCC_AST_OPT_H */
