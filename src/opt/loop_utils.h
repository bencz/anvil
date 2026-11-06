#ifndef ANVIL_OPT_LOOP_UTILS_H
#define ANVIL_OPT_LOOP_UTILS_H

#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"

/* The loop must have an outside predecessor. On success, rebuild CFG and loop
 * snapshots before further queries. Allocation failures leave live IR intact. */
anvil_block_t *anvil_opt_create_preheader(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop);
/* Every edge of `terminator` to `from` leads to `to` instead. */
void anvil_opt_redirect_edge(anvil_instr_t *terminator, anvil_block_t *from, anvil_block_t *to);

/* loop_strength.c, after the optimization pipeline
 * (anvil_opt_prepare_codegen): pointer streams for targets whose accesses
 * advance their own base, then the counters they leave count to zero. */
anvil_pass_result_t anvil_opt_loop_strength_late(anvil_func_t *func);

#endif
