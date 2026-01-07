#ifndef ANVIL_IR_OPT_H
#define ANVIL_IR_OPT_H

#include "../ir/module.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnvilOptStats {
    int constants_folded;
    int dead_code_eliminated;
    int branches_simplified;
    int instructions_removed;
} AnvilOptStats;

bool anvil_opt_const_fold(AnvilFunc* func, AnvilOptStats* stats);

bool anvil_opt_dce(AnvilFunc* func, AnvilOptStats* stats);

bool anvil_opt_simplify_cfg(AnvilFunc* func, AnvilOptStats* stats);

bool anvil_opt_mem2reg(AnvilFunc* func, AnvilOptStats* stats);

void anvil_opt_run_all(AnvilModule* mod, int opt_level, AnvilOptStats* stats);

#ifdef __cplusplus
}
#endif

#endif
