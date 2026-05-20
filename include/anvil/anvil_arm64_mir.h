/*
 * ANVIL - ARM64 reference MachineIR lowering/emission helpers.
 *
 * ARM64 is the current reference implementation for the generic MachineIR
 * backend path. These helpers are exposed for regression tests and for future
 * backend work that wants to follow the same lower/verify/allocate/emit flow.
 */

#ifndef ANVIL_ARM64_MIR_H
#define ANVIL_ARM64_MIR_H

#include "anvil.h"
#include "anvil_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

anvil_mir_func_t *anvil_arm64_lower_func_to_mir(anvil_func_t *func);
bool anvil_arm64_verify_mir_legal(const anvil_mir_func_t *mir,
                                  char *error,
                                  size_t error_len);
bool anvil_arm64_regalloc_mir(anvil_mir_func_t *mir);
bool anvil_arm64_emit_mir(const anvil_mir_func_t *mir,
                          char **output,
                          size_t *len);
bool anvil_arm64_emit_mir_abi(const anvil_mir_func_t *mir,
                              anvil_abi_t abi,
                              char **output,
                              size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_ARM64_MIR_H */
