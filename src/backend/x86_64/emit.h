#ifndef ANVIL_X86_64_EMIT_H
#define ANVIL_X86_64_EMIT_H

#include "../backend.h"
#include "regs.h"

#ifdef __cplusplus
extern "C" {
#endif

void x86_64_emit_prologue(AnvilBackend* backend, AnvilMFunc* func, AnvilAsmBuffer* out);
void x86_64_emit_epilogue(AnvilBackend* backend, AnvilMFunc* func, AnvilAsmBuffer* out);
void x86_64_emit_instruction(AnvilBackend* backend, AnvilMInst* inst, AnvilAsmBuffer* out);
void x86_64_emit_label(AnvilBackend* backend, const char* label, AnvilAsmBuffer* out);
void x86_64_emit_data(AnvilBackend* backend, void* data, AnvilAsmBuffer* out);

void x86_64_emit_func(AnvilBackend* backend, AnvilMFunc* func, AnvilAsmBuffer* out);
void x86_64_emit_mir(AnvilBackend* backend, AnvilMIR* mir, AnvilAsmBuffer* out);

#ifdef __cplusplus
}
#endif

#endif
