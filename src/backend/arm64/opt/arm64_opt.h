/*
 * ANVIL - ARM64 Backend Optimizations
 * 
 * Architecture-specific optimization passes that run during prepare_ir phase.
 * These optimizations are applied after generic IR optimizations (src/opt)
 * and before code generation.
 */

#ifndef ARM64_OPT_H
#define ARM64_OPT_H

#include "../arm64_internal.h"

/* ============================================================================
 * Optimization Pass Interface
 * ============================================================================ */

/* Run all ARM64-specific optimizations on a module */
void arm64_opt_module(arm64_backend_t *be, anvil_module_t *mod);

/* Run all ARM64-specific optimizations on a function */
void arm64_opt_function(arm64_backend_t *be, anvil_func_t *func);

/* ============================================================================
 * Individual Optimization Passes
 * ============================================================================ */

/* Peephole optimizations on IR
 * - Combine adjacent loads/stores
 * - Eliminate redundant moves
 * - Simplify address calculations
 */
void arm64_opt_peephole(arm64_backend_t *be, anvil_func_t *func);

/* Comparison/branch optimization
 * - Combine cmp + cset + cbnz into cmp + b.cond
 * - Use cbz/cbnz for zero comparisons
 * - Use tbz/tbnz for bit tests
 */
void arm64_opt_branch(arm64_backend_t *be, anvil_func_t *func);

#endif /* ARM64_OPT_H */
