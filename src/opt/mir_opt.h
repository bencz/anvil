#ifndef ANVIL_MIR_OPT_H
#define ANVIL_MIR_OPT_H

#include "../mir/mir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnvilMirOptStats {
    int redundant_moves_removed;
    int identity_ops_removed;
    int strength_reductions;
    int instructions_combined;
    int copy_propagations;
    int dead_code_removed;
} AnvilMirOptStats;

bool anvil_mir_peephole(AnvilMFunc* func, AnvilMirOptStats* stats);
bool anvil_mir_remove_redundant_moves(AnvilMFunc* func, AnvilMirOptStats* stats);
bool anvil_mir_strength_reduce(AnvilMFunc* func, AnvilMirOptStats* stats);
bool anvil_mir_copy_propagation(AnvilMFunc* func, AnvilMirOptStats* stats);
bool anvil_mir_dead_code_elimination(AnvilMFunc* func, AnvilMirOptStats* stats);
bool anvil_mir_eliminate_move_chains(AnvilMFunc* func, AnvilMirOptStats* stats);
bool anvil_mir_fold_mov_op_mov(AnvilMFunc* func, AnvilMirOptStats* stats);

void anvil_mir_analyze_function(AnvilMFunc* func);

void anvil_mir_opt_run_all(AnvilMIR* mir, int opt_level, AnvilMirOptStats* stats);

#ifdef __cplusplus
}
#endif

#endif
