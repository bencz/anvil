/* SSE lane-wise operations (and the scalar FP ones the operation table
 * prices) as the i386 (gas32.c) and x86-64 (gas64.c) GAS printers both spell
 * them: one table (x86_sse_isa), one printer, parameterized by the register
 * names and the printer's XMM temporaries. */
#ifndef ANVIL_X86_SSE_VECTOR_H
#define ANVIL_X86_SSE_VECTOR_H

#include "anvil/anvil_internal.h"
#include "anvil/anvil_machine.h"
#include "../../common/vector_isa.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    anvil_strbuf_t *code;
    const anvil_mir_func_t *mir;
    const char *const *xmm;     /* XMM names by register number */
    int num_xmm;
    const char *const *gpr32;   /* 32-bit general register names */
    const char *const *gpr64;   /* 64-bit ones; NULL where there are none */
    /* XMM registers the printer may overwrite at any instruction (`temp`)
     * and at those declaring it as scratch (`temp2`: reductions and
     * accumulations). */
    const char *temp, *temp2;
    bool failed;
} x86_sse_printer_t;

/* `dst = a OP b` with a destructive two-operand instruction (`OP b, dst`):
 * a commutative operation whose result shares b's register applies a to
 * it, any other goes through `temp`. */
void x86_sse_emit_destructive(x86_sse_printer_t *p, const char *mnemonic, anvil_mir_vreg_t def, anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs,
                              bool commutative);

/* MIR VECTOR or SCALAR instruction `index` (an FP or lane operation of the
 * table, not the general-register ones such as rotates or MULH), as `isa`
 * names it. */
void x86_sse_emit_vector(x86_sse_printer_t *p, const anvil_vector_isa_t *isa, size_t index, const anvil_mir_instr_info_t *info);

/* The operations, lane widths and CPU features of SSE through SSE4.2 and
 * FMA3 on 128-bit vectors, and the scalar rows x86-64 prices. */
extern const anvil_vector_isa_t x86_sse_isa[];
extern const size_t x86_sse_isa_count;

#endif
