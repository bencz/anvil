#ifndef ANVIL_PPC64_PEEPHOLE_H
#define ANVIL_PPC64_PEEPHOLE_H

#include "../../backend.h"

#ifdef __cplusplus
extern "C" {
#endif

bool ppc64_peephole_redundant_moves(AnvilMFunc* func);
bool ppc64_peephole_copy_propagation(AnvilMFunc* func);
bool ppc64_peephole_eliminate_move_chains(AnvilMFunc* func);
bool ppc64_peephole_strength_reduce(AnvilMFunc* func);
bool ppc64_peephole_combine_instructions(AnvilMFunc* func);

void ppc64_peephole_run_all(AnvilBackend* backend, AnvilMFunc* func);

#ifdef __cplusplus
}
#endif

#endif
