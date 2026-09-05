/* Immutable, reference-counted CFG snapshots. build validates the function's
 * topology before reusing its cache. An acquired snapshot remains valid until
 * destroy, even if another query rebuilds the cache after a CFG mutation. */
#ifndef ANVIL_ANALYSIS_H
#define ANVIL_ANALYSIS_H

#include "anvil/anvil_internal.h"

typedef struct anvil_cfg {
    anvil_block_t **blocks;
    size_t count;
    size_t entry;
    size_t *successor_offsets;
    size_t *successors;
    size_t *predecessor_offsets;
    size_t *predecessors;
    size_t *rpo; /* Reachable RPO prefix, then unreachable blocks in list order. */
    size_t reachable_count;
    size_t *rpo_rank;
    size_t *idom;
    size_t *block_map;
    size_t map_capacity;
    size_t *source_offsets;
    size_t *source_targets;
    size_t *references;
} anvil_opt_cfg_t;

bool anvil_opt_cfg_build(anvil_func_t *func, anvil_opt_cfg_t *cfg);
void anvil_opt_cfg_destroy(anvil_opt_cfg_t *cfg);
size_t anvil_opt_cfg_index(const anvil_opt_cfg_t *cfg, const anvil_block_t *block);
bool anvil_opt_cfg_dominates(const anvil_opt_cfg_t *cfg, size_t dominator, size_t block);
void anvil_func_invalidate_cfg(anvil_func_t *func);

/* Derived analyses use the snapshot's dense block indices. Keep the CFG alive
 * while querying them, and rebuild after changing control flow. */
typedef struct {
    size_t *offsets;
    size_t *blocks;
    size_t count;
} anvil_dominance_frontier_t;

bool anvil_dominance_frontier_build(const anvil_opt_cfg_t *cfg, anvil_dominance_frontier_t *frontier);
void anvil_dominance_frontier_destroy(anvil_dominance_frontier_t *frontier);

typedef struct {
    size_t header;
    size_t preheader;
    size_t parent;
    size_t depth;
    size_t member_count;
    size_t latch_count;
    size_t exit_edge_count;
} anvil_loop_info_t;

typedef struct {
    anvil_loop_info_t *loops;
    uint64_t *members;
    size_t count;
    size_t block_count;
    size_t words;
} anvil_loop_analysis_t;

/* Natural loops only: all backedges must be dominated by their header.
 * Irreducible cycles do not acquire an invented single-entry loop header. */
bool anvil_loop_analysis_build(const anvil_opt_cfg_t *cfg, anvil_loop_analysis_t *analysis);
bool anvil_loop_contains(const anvil_loop_analysis_t *analysis, size_t loop, size_t block);
void anvil_loop_analysis_destroy(anvil_loop_analysis_t *analysis);

/* Def-use snapshot indexed by live instructions, independent of sparse IDs.
 * Rebuild after replacing operands or inserting/removing instructions. */
typedef struct {
    anvil_instr_t **instructions;
    size_t count;
    size_t *use_offsets;
    size_t *users;
    size_t *value_map;
    size_t map_capacity;
} anvil_def_use_t;

bool anvil_def_use_build(anvil_func_t *func, anvil_def_use_t *graph);
void anvil_def_use_destroy(anvil_def_use_t *graph);
size_t anvil_def_use_definition(const anvil_def_use_t *graph, const anvil_value_t *value);

typedef enum {
    ANVIL_ALIAS_MAY,
    ANVIL_ALIAS_NO,
    ANVIL_ALIAS_MUST,
} anvil_alias_result_t;

/* Constant address walks and bounded allocation identity. Unknown offsets,
 * escaping pointer values and unsupported address expressions stay MAY. */
anvil_alias_result_t anvil_memory_alias(const anvil_value_t *left, size_t left_size, const anvil_value_t *right, size_t right_size);
/* Succeeds only for a complete range inside a known stack/global object. */
bool anvil_memory_bounded_range(const anvil_value_t *pointer, size_t size, const anvil_value_t **object, size_t *offset);

#endif
