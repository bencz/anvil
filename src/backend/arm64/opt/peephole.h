#ifndef ANVIL_ARM64_PEEPHOLE_H
#define ANVIL_ARM64_PEEPHOLE_H

#include "../../backend.h"

#ifdef __cplusplus
extern "C" {
#endif

bool arm64_peephole_remove_redundant_moves(AnvilMFunc* func);
bool arm64_peephole_fold_mov_op_mov(AnvilMFunc* func);

void arm64_peephole_run_all(AnvilBackend* backend, AnvilMFunc* func);

#ifdef __cplusplus
}
#endif

#endif
