#ifndef ANVIL_LIVENESS_H
#define ANVIL_LIVENESS_H

#include "mir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnvilLivenessInfo {
    AnvilVec* live_in;
    AnvilVec* live_out;
    int num_blocks;
} AnvilLivenessInfo;

AnvilLivenessInfo* anvil_liveness_compute(AnvilMFunc* func);
void anvil_liveness_free(AnvilLivenessInfo* info);

bool anvil_liveness_is_live_at(AnvilLivenessInfo* info, int vreg, AnvilMBlock* block);
void anvil_liveness_get_live_range(AnvilMFunc* func, int vreg, int* first_use, int* last_use);

#ifdef __cplusplus
}
#endif

#endif
