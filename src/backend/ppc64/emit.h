#ifndef ANVIL_PPC64_EMIT_H
#define ANVIL_PPC64_EMIT_H

#include "../backend.h"
#include "regs.h"

#ifdef __cplusplus
extern "C" {
#endif

void ppc64_emit_instruction(AnvilBackend* backend, AnvilMInst* inst, AnvilAsmBuffer* out);
void ppc64_emit_prologue(AnvilBackend* backend, AnvilMFunc* func, AnvilAsmBuffer* out);
void ppc64_emit_epilogue(AnvilBackend* backend, AnvilMFunc* func, AnvilAsmBuffer* out);

#ifdef __cplusplus
}
#endif

#endif
