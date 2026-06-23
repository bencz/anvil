/*
 * ANVIL - x86 (32-bit) MachineIR lowering/emission helpers.
 *
 * x86 follows the shared MachineIR/regalloc path used by the ARM64 and x86-64
 * reference backends: lower source IR into MachineIR, validate target legality,
 * allocate registers, materialize spills, and emit assembly. Calling
 * conventions (cdecl, stdcall, fastcall) and platform decoration (ELF, Mach-O,
 * Win32 COFF) are selected through a target descriptor table.
 */

#ifndef ANVIL_X86_MIR_H
#define ANVIL_X86_MIR_H

#include "anvil.h"
#include "anvil_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

anvil_mir_func_t *anvil_x86_lower_func_to_mir(anvil_func_t *func);
bool anvil_x86_verify_mir_legal(const anvil_mir_func_t *mir,
                                char *error,
                                size_t error_len);
bool anvil_x86_regalloc_mir(anvil_mir_func_t *mir);
bool anvil_x86_emit_mir(const anvil_mir_func_t *mir,
                        char **output,
                        size_t *len);
bool anvil_x86_emit_mir_abi(const anvil_mir_func_t *mir,
                            anvil_func_t *func,
                            anvil_abi_t abi,
                            anvil_syntax_t syntax,
                            char **output,
                            size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_X86_MIR_H */
