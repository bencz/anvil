/*
 * ANVIL - PowerPC 64-bit Backend CPU-Specific Code Generation
 * 
 * This module provides CPU model-aware code generation for PPC64.
 * It emits optimized instructions when available on the target CPU,
 * and falls back to emulation sequences for older processors.
 */

#include "ppc64_internal.h"
#include <stdio.h>

/* ============================================================================
 * CPU Feature Detection
 * ============================================================================ */

bool ppc64_has_feature(ppc64_backend_t *be, anvil_cpu_features_t feature)
{
    if (!be || !be->ctx) return false;
    return anvil_ctx_has_feature(be->ctx, feature);
}

anvil_cpu_model_t ppc64_get_cpu_model(ppc64_backend_t *be)
{
    if (!be || !be->ctx) return ANVIL_CPU_GENERIC;
    return anvil_ctx_get_cpu(be->ctx);
}

bool ppc64_can_use_altivec(ppc64_backend_t *be)
{
    return ppc64_has_feature(be, ANVIL_FEATURE_PPC_ALTIVEC);
}

bool ppc64_can_use_vsx(ppc64_backend_t *be)
{
    return ppc64_has_feature(be, ANVIL_FEATURE_PPC_VSX);
}

bool ppc64_can_use_pcrel(ppc64_backend_t *be)
{
    return ppc64_has_feature(be, ANVIL_FEATURE_PPC_PCREL);
}

bool ppc64_can_use_mma(ppc64_backend_t *be)
{
    return ppc64_has_feature(be, ANVIL_FEATURE_PPC_MMA);
}

/* ============================================================================
 * CPU Directive Emission
 * ============================================================================ */

void ppc64_emit_cpu_directive(ppc64_backend_t *be)
{
    anvil_cpu_model_t cpu = ppc64_get_cpu_model(be);
    
    /* Emit .machine directive based on CPU model */
    switch (cpu) {
        case ANVIL_CPU_PPC64_970:
        case ANVIL_CPU_PPC64_970FX:
        case ANVIL_CPU_PPC64_970MP:
            anvil_strbuf_append(&be->code, "\t.machine \"ppc970\"\n");
            break;
        case ANVIL_CPU_PPC64_POWER4:
        case ANVIL_CPU_PPC64_POWER4P:
            anvil_strbuf_append(&be->code, "\t.machine \"power4\"\n");
            break;
        case ANVIL_CPU_PPC64_POWER5:
        case ANVIL_CPU_PPC64_POWER5P:
            anvil_strbuf_append(&be->code, "\t.machine \"power5\"\n");
            break;
        case ANVIL_CPU_PPC64_POWER6:
            anvil_strbuf_append(&be->code, "\t.machine \"power6\"\n");
            break;
        case ANVIL_CPU_PPC64_POWER7:
            anvil_strbuf_append(&be->code, "\t.machine \"power7\"\n");
            break;
        case ANVIL_CPU_PPC64_POWER8:
            anvil_strbuf_append(&be->code, "\t.machine \"power8\"\n");
            break;
        case ANVIL_CPU_PPC64_POWER9:
            anvil_strbuf_append(&be->code, "\t.machine \"power9\"\n");
            break;
        case ANVIL_CPU_PPC64_POWER10:
            anvil_strbuf_append(&be->code, "\t.machine \"power10\"\n");
            break;
        default:
            /* Generic PPC64 - no specific machine directive */
            break;
    }
}

/* ============================================================================
 * Population Count (popcnt)
 * ============================================================================
 * POWER5+ has native popcntd instruction
 * Older CPUs need emulation via bit manipulation
 */

void ppc64_emit_popcnt(ppc64_backend_t *be, int dest_reg, int src_reg)
{
    if (ppc64_has_feature(be, ANVIL_FEATURE_PPC_POPCNTD)) {
        /* Use native popcntd instruction (POWER5+) */
        anvil_strbuf_appendf(&be->code, "\tpopcntd %s, %s\n",
            ppc64_gpr_names[dest_reg], ppc64_gpr_names[src_reg]);
    } else {
        /* Emulation for older CPUs using parallel bit counting */
        /* This is the classic divide-and-conquer popcount algorithm */
        
        const char *d = ppc64_gpr_names[dest_reg];
        const char *s = ppc64_gpr_names[src_reg];
        const char *t1 = ppc64_gpr_names[PPC64_R11];  /* temp register */
        const char *t2 = ppc64_gpr_names[PPC64_R12];  /* temp register */
        
        anvil_strbuf_appendf(&be->code, "\t# popcnt emulation for %s -> %s\n", s, d);
        
        /* Copy source to dest if different */
        if (dest_reg != src_reg) {
            anvil_strbuf_appendf(&be->code, "\tmr %s, %s\n", d, s);
        }
        
        /* v = v - ((v >> 1) & 0x5555555555555555) */
        anvil_strbuf_appendf(&be->code, "\tsrdi %s, %s, 1\n", t1, d);
        anvil_strbuf_appendf(&be->code, "\tlis %s, 0x5555\n", t2);
        anvil_strbuf_appendf(&be->code, "\tori %s, %s, 0x5555\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\tsldi %s, %s, 32\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\toris %s, %s, 0x5555\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\tori %s, %s, 0x5555\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\tand %s, %s, %s\n", t1, t1, t2);
        anvil_strbuf_appendf(&be->code, "\tsub %s, %s, %s\n", d, d, t1);
        
        /* v = (v & 0x3333...) + ((v >> 2) & 0x3333...) */
        anvil_strbuf_appendf(&be->code, "\tlis %s, 0x3333\n", t2);
        anvil_strbuf_appendf(&be->code, "\tori %s, %s, 0x3333\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\tsldi %s, %s, 32\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\toris %s, %s, 0x3333\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\tori %s, %s, 0x3333\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\tand %s, %s, %s\n", t1, d, t2);
        anvil_strbuf_appendf(&be->code, "\tsrdi %s, %s, 2\n", d, d);
        anvil_strbuf_appendf(&be->code, "\tand %s, %s, %s\n", d, d, t2);
        anvil_strbuf_appendf(&be->code, "\tadd %s, %s, %s\n", d, d, t1);
        
        /* v = (v + (v >> 4)) & 0x0F0F... */
        anvil_strbuf_appendf(&be->code, "\tsrdi %s, %s, 4\n", t1, d);
        anvil_strbuf_appendf(&be->code, "\tadd %s, %s, %s\n", d, d, t1);
        anvil_strbuf_appendf(&be->code, "\tlis %s, 0x0F0F\n", t2);
        anvil_strbuf_appendf(&be->code, "\tori %s, %s, 0x0F0F\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\tsldi %s, %s, 32\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\toris %s, %s, 0x0F0F\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\tori %s, %s, 0x0F0F\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\tand %s, %s, %s\n", d, d, t2);
        
        /* Multiply by 0x0101... and shift right 56 */
        anvil_strbuf_appendf(&be->code, "\tlis %s, 0x0101\n", t2);
        anvil_strbuf_appendf(&be->code, "\tori %s, %s, 0x0101\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\tsldi %s, %s, 32\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\toris %s, %s, 0x0101\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\tori %s, %s, 0x0101\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\tmulld %s, %s, %s\n", d, d, t2);
        anvil_strbuf_appendf(&be->code, "\tsrdi %s, %s, 56\n", d, d);
    }
}

/* ============================================================================
 * Byte Swap (bswap)
 * ============================================================================
 * POWER7+ has ldbrx/stdbrx for memory byte-reversal
 * For register-to-register, we need manual swap on all CPUs
 */

void ppc64_emit_bswap64(ppc64_backend_t *be, int dest_reg, int src_reg)
{
    const char *d = ppc64_gpr_names[dest_reg];
    const char *s = ppc64_gpr_names[src_reg];
    const char *t1 = ppc64_gpr_names[PPC64_R11];
    const char *t2 = ppc64_gpr_names[PPC64_R12];
    
    anvil_strbuf_appendf(&be->code, "\t# bswap64 %s -> %s\n", s, d);
    
    /* Store to stack, load with byte reversal if available */
    if (ppc64_has_feature(be, ANVIL_FEATURE_PPC_LDBRX)) {
        /* Use memory-based byte reversal (POWER7+) */
        anvil_strbuf_appendf(&be->code, "\tstd %s, -8(r1)\n", s);
        anvil_strbuf_appendf(&be->code, "\taddi %s, r1, -8\n", t1);
        anvil_strbuf_appendf(&be->code, "\tldbrx %s, 0, %s\n", d, t1);
    } else {
        /* Manual byte swap using rotates and masks */
        /* Swap bytes within each 32-bit half, then swap halves */
        
        if (dest_reg != src_reg) {
            anvil_strbuf_appendf(&be->code, "\tmr %s, %s\n", d, s);
        }
        
        /* Rotate left 8, mask and combine to swap adjacent bytes */
        anvil_strbuf_appendf(&be->code, "\trldicl %s, %s, 32, 0\n", t1, d);  /* swap 32-bit halves */
        
        /* Swap bytes within each 32-bit word */
        anvil_strbuf_appendf(&be->code, "\trlwinm %s, %s, 24, 0, 31\n", t2, t1);
        anvil_strbuf_appendf(&be->code, "\trlwimi %s, %s, 8, 8, 15\n", t2, t1);
        anvil_strbuf_appendf(&be->code, "\trlwimi %s, %s, 8, 24, 31\n", t2, t1);
        
        /* Do the same for the other half */
        anvil_strbuf_appendf(&be->code, "\trldicl %s, %s, 32, 32\n", t1, t1);
        anvil_strbuf_appendf(&be->code, "\trlwinm %s, %s, 24, 0, 31\n", d, t1);
        anvil_strbuf_appendf(&be->code, "\trlwimi %s, %s, 8, 8, 15\n", d, t1);
        anvil_strbuf_appendf(&be->code, "\trlwimi %s, %s, 8, 24, 31\n", d, t1);
        
        /* Combine the two halves */
        anvil_strbuf_appendf(&be->code, "\tsldi %s, %s, 32\n", t2, t2);
        anvil_strbuf_appendf(&be->code, "\tor %s, %s, %s\n", d, d, t2);
    }
}

void ppc64_emit_bswap32(ppc64_backend_t *be, int dest_reg, int src_reg)
{
    const char *d = ppc64_gpr_names[dest_reg];
    const char *s = ppc64_gpr_names[src_reg];
    const char *t1 = ppc64_gpr_names[PPC64_R11];
    
    anvil_strbuf_appendf(&be->code, "\t# bswap32 %s -> %s\n", s, d);
    
    if (ppc64_has_feature(be, ANVIL_FEATURE_PPC_LDBRX)) {
        /* Use memory-based byte reversal (POWER7+) */
        anvil_strbuf_appendf(&be->code, "\tstw %s, -4(r1)\n", s);
        anvil_strbuf_appendf(&be->code, "\taddi %s, r1, -4\n", t1);
        anvil_strbuf_appendf(&be->code, "\tlwbrx %s, 0, %s\n", d, t1);
    } else {
        /* Manual 32-bit byte swap */
        anvil_strbuf_appendf(&be->code, "\trlwinm %s, %s, 24, 0, 31\n", d, s);
        anvil_strbuf_appendf(&be->code, "\trlwimi %s, %s, 8, 8, 15\n", d, s);
        anvil_strbuf_appendf(&be->code, "\trlwimi %s, %s, 8, 24, 31\n", d, s);
    }
}

/* ============================================================================
 * Conditional Select (isel)
 * ============================================================================
 * POWER7+ has isel instruction for branchless conditional select
 * Older CPUs need branch-based selection
 */

void ppc64_emit_isel(ppc64_backend_t *be, int dest_reg, int true_reg, int false_reg, int cr_bit)
{
    const char *d = ppc64_gpr_names[dest_reg];
    const char *t = ppc64_gpr_names[true_reg];
    const char *f = ppc64_gpr_names[false_reg];
    
    if (ppc64_has_feature(be, ANVIL_FEATURE_PPC_ISEL)) {
        /* Use native isel instruction (POWER7+) */
        anvil_strbuf_appendf(&be->code, "\tisel %s, %s, %s, %d\n", d, t, f, cr_bit);
    } else {
        /* Emulation using conditional branch */
        int skip_label = be->label_counter++;
        
        anvil_strbuf_appendf(&be->code, "\t# isel emulation\n");
        anvil_strbuf_appendf(&be->code, "\tmr %s, %s\n", d, f);  /* Default to false */
        anvil_strbuf_appendf(&be->code, "\tbc 4, %d, .Lisel%d\n", cr_bit, skip_label);  /* Branch if false */
        anvil_strbuf_appendf(&be->code, "\tmr %s, %s\n", d, t);  /* Select true value */
        anvil_strbuf_appendf(&be->code, ".Lisel%d:\n", skip_label);
    }
}

/* ============================================================================
 * Compare Bytes (cmpb)
 * ============================================================================
 * POWER6+ has cmpb instruction for parallel byte comparison
 * Older CPUs need byte-by-byte comparison
 */

void ppc64_emit_cmpb(ppc64_backend_t *be, int dest_reg, int src1_reg, int src2_reg)
{
    const char *d = ppc64_gpr_names[dest_reg];
    const char *s1 = ppc64_gpr_names[src1_reg];
    const char *s2 = ppc64_gpr_names[src2_reg];
    
    if (ppc64_has_feature(be, ANVIL_FEATURE_PPC_CMPB)) {
        /* Use native cmpb instruction (POWER6+) */
        anvil_strbuf_appendf(&be->code, "\tcmpb %s, %s, %s\n", d, s1, s2);
    } else {
        /* Emulation: XOR and then set each byte to 0xFF if zero, 0x00 otherwise */
        const char *t1 = ppc64_gpr_names[PPC64_R11];
        const char *t2 = ppc64_gpr_names[PPC64_R12];
        
        anvil_strbuf_appendf(&be->code, "\t# cmpb emulation\n");
        anvil_strbuf_appendf(&be->code, "\txor %s, %s, %s\n", d, s1, s2);
        
        /* For each byte, if it's 0, set to 0xFF, else 0x00 */
        /* This is complex - use a loop-like approach with masks */
        anvil_strbuf_appendf(&be->code, "\tli %s, 0\n", t1);  /* Result accumulator */
        
        /* Process each byte (simplified - full implementation would be longer) */
        for (int i = 0; i < 8; i++) {
            int shift = i * 8;
            anvil_strbuf_appendf(&be->code, "\trldicl %s, %s, %d, 56\n", t2, d, 64 - shift);
            anvil_strbuf_appendf(&be->code, "\tcmpdi cr0, %s, 0\n", t2);
            anvil_strbuf_appendf(&be->code, "\tli %s, 0\n", t2);
            anvil_strbuf_appendf(&be->code, "\tbeq cr0, .Lcmpb_set%d_%d\n", be->label_counter, i);
            anvil_strbuf_appendf(&be->code, "\tb .Lcmpb_done%d_%d\n", be->label_counter, i);
            anvil_strbuf_appendf(&be->code, ".Lcmpb_set%d_%d:\n", be->label_counter, i);
            anvil_strbuf_appendf(&be->code, "\tli %s, 0xFF\n", t2);
            anvil_strbuf_appendf(&be->code, ".Lcmpb_done%d_%d:\n", be->label_counter, i);
            if (shift > 0) {
                anvil_strbuf_appendf(&be->code, "\tsldi %s, %s, %d\n", t2, t2, shift);
            }
            anvil_strbuf_appendf(&be->code, "\tor %s, %s, %s\n", t1, t1, t2);
        }
        be->label_counter++;
        
        anvil_strbuf_appendf(&be->code, "\tmr %s, %s\n", d, t1);
    }
}

/* ============================================================================
 * FP Copy Sign (fcpsgn)
 * ============================================================================
 * POWER7+ has fcpsgn instruction
 * Older CPUs need manual sign manipulation
 */

void ppc64_emit_fcpsgn(ppc64_backend_t *be, int dest_fpr, int sign_fpr, int mag_fpr)
{
    if (ppc64_has_feature(be, ANVIL_FEATURE_PPC_FCPSGN)) {
        /* Use native fcpsgn instruction (POWER7+) */
        anvil_strbuf_appendf(&be->code, "\tfcpsgn f%d, f%d, f%d\n", dest_fpr, sign_fpr, mag_fpr);
    } else {
        /* Emulation using fabs and fneg */
        anvil_strbuf_appendf(&be->code, "\t# fcpsgn emulation\n");
        anvil_strbuf_appendf(&be->code, "\tfabs f%d, f%d\n", dest_fpr, mag_fpr);
        
        /* Check sign of sign_fpr and negate if negative */
        /* This requires moving to GPR, checking sign bit, and conditionally negating */
        anvil_strbuf_appendf(&be->code, "\tstfd f%d, -8(r1)\n", sign_fpr);
        anvil_strbuf_appendf(&be->code, "\tld r11, -8(r1)\n");
        anvil_strbuf_appendf(&be->code, "\tsrdi r11, r11, 63\n");  /* Get sign bit */
        anvil_strbuf_appendf(&be->code, "\tcmpdi cr0, r11, 0\n");
        
        int skip_label = be->label_counter++;
        anvil_strbuf_appendf(&be->code, "\tbeq cr0, .Lfcpsgn%d\n", skip_label);
        anvil_strbuf_appendf(&be->code, "\tfneg f%d, f%d\n", dest_fpr, dest_fpr);
        anvil_strbuf_appendf(&be->code, ".Lfcpsgn%d:\n", skip_label);
    }
}
