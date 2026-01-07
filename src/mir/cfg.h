#ifndef ANVIL_CFG_H
#define ANVIL_CFG_H

#include "mir.h"

#ifdef __cplusplus
extern "C" {
#endif

void anvil_cfg_build(AnvilMFunc* func);
void anvil_cfg_compute_dominators(AnvilMFunc* func);
void anvil_cfg_compute_post_order(AnvilMFunc* func, AnvilVec* order);
void anvil_cfg_compute_reverse_post_order(AnvilMFunc* func, AnvilVec* order);

#ifdef __cplusplus
}
#endif

#endif
