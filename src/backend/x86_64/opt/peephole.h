#ifndef ANVIL_X86_64_PEEPHOLE_H
#define ANVIL_X86_64_PEEPHOLE_H

#include "../../backend.h"

#ifdef __cplusplus
extern "C" {
#endif

bool x86_64_peephole_remove_redundant_moves(AnvilMFunc* func);
bool x86_64_peephole_eliminate_frame_for_leaf(AnvilMFunc* func);

void x86_64_peephole_run_all(AnvilBackend* backend, AnvilMFunc* func);

#ifdef __cplusplus
}
#endif

#endif
