/* The SSE printer both x86 GAS printers share (see x86_sse_vector.h). */
#include "x86_sse_vector.h"

#include <stdio.h>
#include <string.h>

static int sse_phys(x86_sse_printer_t *p, anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *assignment = vreg == ANVIL_MIR_NO_VREG ? NULL : anvil_mir_get_assignment(p->mir, vreg);
    if (!assignment || assignment->spilled || assignment->phys_reg < 0) {
        p->failed = true;
        return -1;
    }
    return assignment->phys_reg;
}

static const char *sse_xmm(x86_sse_printer_t *p, anvil_mir_vreg_t vreg)
{
    int phys = sse_phys(p, vreg);
    if (phys < 0 || phys >= p->num_xmm) {
        p->failed = true;
        return "?";
    }
    return p->xmm[phys];
}

/* The general register `phys` with `bits` (64 only where the target has
 * them). */
static const char *sse_gpr(x86_sse_printer_t *p, int phys, unsigned bits)
{
    const char *const *names = bits > 32 ? p->gpr64 : p->gpr32;
    if (!names || phys < 0) {
        p->failed = true;
        return "?";
    }
    return names[phys];
}

void x86_sse_emit_destructive(x86_sse_printer_t *p, const char *mnemonic, anvil_mir_vreg_t def, anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs,
                              bool commutative)
{
    const char *dst = sse_xmm(p, def), *a = sse_xmm(p, lhs), *b = sse_xmm(p, rhs);
    if (p->failed)
        return;
    bool overlaps_rhs = strcmp(dst, b) == 0 && strcmp(a, b) != 0;
    if (overlaps_rhs && commutative) {
        anvil_strbuf_appendf(p->code, "\t%s %%%s, %%%s\n", mnemonic, a, dst);
        return;
    }
    const char *work = overlaps_rhs ? p->temp : dst;
    if (strcmp(a, work) != 0)
        anvil_strbuf_appendf(p->code, "\tmovaps %%%s, %%%s\n", a, work);
    anvil_strbuf_appendf(p->code, "\t%s %%%s, %%%s\n", mnemonic, b, work);
    if (overlaps_rhs)
        anvil_strbuf_appendf(p->code, "\tmovaps %%%s, %%%s\n", work, dst);
}

/* Broadcast of lane 0.  FP scalars are already in an XMM register; an
 * integer scalar moves from its general register first, and narrow lanes
 * widen to a dword (bytes pair up, words fill the low quadword) before
 * pshufd. */
static void sse_splat(x86_sse_printer_t *p, const anvil_vector_isa_t *isa, const anvil_mir_instr_info_t *info, anvil_mir_vreg_t scalar)
{
    const char *dst = sse_xmm(p, info->def);
    const anvil_mir_vreg_info_t *source = anvil_mir_get_vreg_info(p->mir, scalar);
    int phys = sse_phys(p, scalar);
    if (p->failed || !source) {
        p->failed = true;
        return;
    }
    if (source->reg_class == ANVIL_MIR_REG_FPR) {
        if (phys >= p->num_xmm) {
            p->failed = true;
            return;
        }
        if (strcmp(dst, p->xmm[phys]) != 0)
            anvil_strbuf_appendf(p->code, "\tmovaps %%%s, %%%s\n", p->xmm[phys], dst);
        if (info->imm == 32)
            anvil_strbuf_appendf(p->code, "\tshufps $0, %%%s, %%%s\n", dst, dst);
        else
            anvil_strbuf_appendf(p->code, "\t%s %%%s, %%%s\n", isa->mnemonic, dst, dst);
        return;
    }
    if (info->imm == 64) {
        const char *wide = sse_gpr(p, phys, 64);
        if (!p->failed)
            anvil_strbuf_appendf(p->code, "\tmovq %%%s, %%%s\n\tpunpcklqdq %%%s, %%%s\n", wide, dst, dst, dst);
        return;
    }
    anvil_strbuf_appendf(p->code, "\tmovd %%%s, %%%s\n", sse_gpr(p, phys, 32), dst);
    if (info->imm == 8)
        anvil_strbuf_appendf(p->code, "\tpunpcklbw %%%s, %%%s\n", dst, dst);
    if (info->imm <= 16)
        anvil_strbuf_appendf(p->code, "\tpshuflw $0, %%%s, %%%s\n", dst, dst);
    anvil_strbuf_appendf(p->code, "\tpshufd $0, %%%s, %%%s\n", dst, dst);
}

static bool sse_commutative(anvil_op_t operation)
{
    return operation == ANVIL_OP_FADD || operation == ANVIL_OP_FMUL || operation == ANVIL_OP_ADD || operation == ANVIL_OP_MUL ||
           operation == ANVIL_OP_AND || operation == ANVIL_OP_OR || operation == ANVIL_OP_XOR || operation == ANVIL_OP_ADD_SAT_U ||
           operation == ANVIL_OP_ADD_SAT_S;
}

/* dst = ~dst, against all ones in the temp. */
static void sse_not(x86_sse_printer_t *p, const char *dst)
{
    anvil_strbuf_appendf(p->code, "\tpcmpeqd %%%s, %%%s\n\tpxor %%%s, %%%s\n", p->temp, p->temp, p->temp, dst);
}

static char sse_lane_suffix(int64_t lane_bits)
{
    return lane_bits == 8 ? 'b' : lane_bits == 16 ? 'w' : lane_bits == 32 ? 'd' : 'q';
}

/* Integer lane compares from pcmpeq/pcmpgt (signed) and the unsigned
 * max/min (a <=u b is max(a, b) == b; a >=u b is min(a, b) == b). */
static void sse_int_compare(x86_sse_printer_t *p, const anvil_vector_isa_t *isa, const anvil_mir_instr_info_t *info, anvil_mir_vreg_t lhs,
                            anvil_mir_vreg_t rhs)
{
    bool complement = false;
    switch (info->ir_op) {
    case ANVIL_OP_CMP_NE:
        complement = true;
        /* fallthrough */
    case ANVIL_OP_CMP_EQ:
        x86_sse_emit_destructive(p, isa->mnemonic, info->def, lhs, rhs, true);
        break;
    case ANVIL_OP_CMP_LE:
        complement = true;
        /* fallthrough */
    case ANVIL_OP_CMP_GT:
        x86_sse_emit_destructive(p, isa->mnemonic, info->def, lhs, rhs, false);
        break;
    case ANVIL_OP_CMP_GE:
        complement = true;
        /* fallthrough */
    case ANVIL_OP_CMP_LT:
        x86_sse_emit_destructive(p, isa->mnemonic, info->def, rhs, lhs, false);
        break;
    default: {
        /* Unsigned order, computed in the temp: inputs stay intact. */
        complement = info->ir_op == ANVIL_OP_CMP_UGT || info->ir_op == ANVIL_OP_CMP_ULT;
        const char *dst = sse_xmm(p, info->def), *a = sse_xmm(p, lhs), *b = sse_xmm(p, rhs);
        if (p->failed)
            return;
        anvil_strbuf_appendf(p->code, "\tmovaps %%%s, %%%s\n\t%s %%%s, %%%s\n\tpcmpeq%c %%%s, %%%s\n\tmovaps %%%s, %%%s\n", a, p->temp, isa->mnemonic,
                             b, p->temp, sse_lane_suffix(info->imm), b, p->temp, p->temp, dst);
        break;
    }
    }
    if (complement && !p->failed)
        sse_not(p, sse_xmm(p, info->def));
}

/* cmpps/cmppd predicate immediates: eq lt le unord neq nlt nle ord. */
static void sse_fcmp(x86_sse_printer_t *p, const anvil_vector_isa_t *isa, const anvil_mir_instr_info_t *info, anvil_mir_vreg_t lhs,
                     anvil_mir_vreg_t rhs)
{
    static const struct {
        int imm;      /* -1: two compares or a constant */
        bool swap;
    } forms[] = {
        [ANVIL_FCMP_OEQ] = {0, false}, [ANVIL_FCMP_OLT] = {1, false}, [ANVIL_FCMP_OLE] = {2, false},
        [ANVIL_FCMP_OGT] = {1, true},  [ANVIL_FCMP_OGE] = {2, true},  [ANVIL_FCMP_UNO] = {3, false},
        [ANVIL_FCMP_UNE] = {4, false}, [ANVIL_FCMP_UGE] = {5, false}, [ANVIL_FCMP_UGT] = {6, false},
        [ANVIL_FCMP_ULT] = {6, true},  [ANVIL_FCMP_ULE] = {5, true},  [ANVIL_FCMP_ORD] = {7, false},
        [ANVIL_FCMP_ONE] = {-1, false}, [ANVIL_FCMP_UEQ] = {-1, false}, [ANVIL_FCMP_FALSE] = {-1, false},
        [ANVIL_FCMP_TRUE] = {-1, false},
    };
    const char *dst = sse_xmm(p, info->def);
    if (p->failed || (unsigned)info->fcmp_pred > (unsigned)ANVIL_FCMP_TRUE) {
        p->failed = true;
        return;
    }
    char mnemonic[24];
    if (info->fcmp_pred == ANVIL_FCMP_FALSE || info->fcmp_pred == ANVIL_FCMP_TRUE) {
        anvil_strbuf_appendf(p->code, "\t%s %%%s, %%%s\n", info->fcmp_pred == ANVIL_FCMP_TRUE ? "pcmpeqd" : "pxor", dst, dst);
        return;
    }
    int imm = forms[info->fcmp_pred].imm;
    if (imm >= 0) {
        /* eq, unord, neq and ord are symmetric. */
        snprintf(mnemonic, sizeof(mnemonic), "%s $%d,", isa->mnemonic, imm);
        bool swap = forms[info->fcmp_pred].swap;
        x86_sse_emit_destructive(p, mnemonic, info->def, swap ? rhs : lhs, swap ? lhs : rhs, imm == 0 || imm == 3 || imm == 4 || imm == 7);
        return;
    }
    /* one = ord & neq, ueq = unord | eq: the first half in the temp, the
     * second (symmetric) in dst. */
    bool one = info->fcmp_pred == ANVIL_FCMP_ONE;
    const char *a = sse_xmm(p, lhs), *b = sse_xmm(p, rhs);
    anvil_strbuf_appendf(p->code, "\tmovaps %%%s, %%%s\n\t%s $%d, %%%s, %%%s\n", a, p->temp, isa->mnemonic, one ? 7 : 3, b, p->temp);
    snprintf(mnemonic, sizeof(mnemonic), "%s $%d,", isa->mnemonic, one ? 4 : 0);
    x86_sse_emit_destructive(p, mnemonic, info->def, lhs, rhs, true);
    anvil_strbuf_appendf(p->code, "\t%s %%%s, %%%s\n", one ? "andps" : "orps", p->temp, dst);
}

/* Lane select: (mask & then) | (~mask & else), the second half in the
 * temp before dst (which may hold an input) is written. */
static void sse_select(x86_sse_printer_t *p, size_t index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t mask = anvil_mir_get_instr_use(p->mir, index, 0);
    anvil_mir_vreg_t then_value = anvil_mir_get_instr_use(p->mir, index, 1);
    anvil_mir_vreg_t else_value = anvil_mir_get_instr_use(p->mir, index, 2);
    const char *dst = sse_xmm(p, info->def), *m = sse_xmm(p, mask), *a = sse_xmm(p, then_value), *b = sse_xmm(p, else_value);
    if (p->failed)
        return;
    anvil_strbuf_appendf(p->code, "\tmovaps %%%s, %%%s\n\tpandn %%%s, %%%s\n", m, p->temp, b, p->temp);
    if (strcmp(dst, m) == 0)
        anvil_strbuf_appendf(p->code, "\tpand %%%s, %%%s\n", a, dst);
    else if (strcmp(dst, a) == 0)
        anvil_strbuf_appendf(p->code, "\tpand %%%s, %%%s\n", m, dst);
    else
        anvil_strbuf_appendf(p->code, "\tmovaps %%%s, %%%s\n\tpand %%%s, %%%s\n", m, dst, a, dst);
    anvil_strbuf_appendf(p->code, "\tpor %%%s, %%%s\n", p->temp, dst);
}

/* A reduction: halves folded with the table's lane operation in the two
 * temporaries (quadwords, then dwords, words and bytes down to the lane
 * width), lane 0 then moved to the result.  A lane mask ORs faster through
 * its sign bits. */
static void sse_reduce(x86_sse_printer_t *p, size_t index, const anvil_vector_isa_t *isa, const anvil_mir_instr_info_t *info,
                       anvil_mir_vreg_t source)
{
    const char *vector = sse_xmm(p, source), *t1 = p->temp, *t2 = p->temp2;
    const anvil_mir_vreg_info_t *result = anvil_mir_get_vreg_info(p->mir, info->def);
    int dst = sse_phys(p, info->def);
    if (p->failed || !result) {
        p->failed = true;
        return;
    }
    if (info->ir_op == ANVIL_OP_REDUCE_OR && anvil_vector_mir_is_mask(p->mir, index, source)) {
        /* All ones in the result when any sign bit is set. */
        const char *low = sse_gpr(p, dst, 32), *whole = p->gpr64 ? sse_gpr(p, dst, 64) : low;
        if (!p->failed)
            anvil_strbuf_appendf(p->code, "\tpmovmskb %%%s, %%%s\n\tnegl %%%s\n\tsbb%c %%%s, %%%s\n", vector, low, low, p->gpr64 ? 'q' : 'l', whole,
                                 whole);
        return;
    }
    const char *m = isa->mnemonic;
    anvil_strbuf_appendf(p->code, "\tpshufd $0x4e, %%%s, %%%s\n\t%s %%%s, %%%s\n", vector, t1, m, vector, t1);
    if (info->imm <= 32)
        anvil_strbuf_appendf(p->code, "\tpshufd $0x55, %%%s, %%%s\n\t%s %%%s, %%%s\n", t1, t2, m, t2, t1);
    if (info->imm <= 16)
        anvil_strbuf_appendf(p->code, "\tpshuflw $0x55, %%%s, %%%s\n\t%s %%%s, %%%s\n", t1, t2, m, t2, t1);
    if (info->imm <= 8)
        anvil_strbuf_appendf(p->code, "\tmovdqa %%%s, %%%s\n\tpsrlw $8, %%%s\n\t%s %%%s, %%%s\n", t1, t2, t2, m, t2, t1);
    if (result->reg_class == ANVIL_MIR_REG_FPR)
        anvil_strbuf_appendf(p->code, "\tmovaps %%%s, %%%s\n", t1, sse_xmm(p, info->def));
    else if (info->imm == 64)
        anvil_strbuf_appendf(p->code, "\tmovq %%%s, %%%s\n", t1, sse_gpr(p, dst, 64));
    else
        anvil_strbuf_appendf(p->code, "\tmovd %%%s, %%%s\n", t1, sse_gpr(p, dst, 32));
}

/* dst = acc + the narrow lanes of x summed into acc's lanes (ACCUM_*): the
 * partial sums of x in the first temporary (the second helps), then one
 * add, so dst may hold acc or x. */
static void sse_accumulate(x86_sse_printer_t *p, const anvil_mir_instr_info_t *info, anvil_mir_vreg_t acc, anvil_mir_vreg_t lanes)
{
    const char *dst = sse_xmm(p, info->def), *a = sse_xmm(p, acc), *x = sse_xmm(p, lanes);
    const char *t1 = p->temp, *t2 = p->temp2;
    if (p->failed)
        return;
    bool sign = info->ir_op == ANVIL_OP_ACCUM_SEXT;
    anvil_strbuf_appendf(p->code, "\tmovdqa %%%s, %%%s\n", x, t1);
    if (!sign && info->widening >= 4) {
        anvil_strbuf_appendf(p->code, "\tpxor %%%s, %%%s\n\tpsadbw %%%s, %%%s\n", t2, t2, t2, t1);
    } else if (!sign) {
        anvil_strbuf_appendf(p->code, "\tmovdqa %%%s, %%%s\n\tpsrld $16, %%%s\n\tpslld $16, %%%s\n\tpsrld $16, %%%s\n\tpaddd %%%s, %%%s\n", x, t2,
                             t1, t2, t2, t2, t1);
    } else if (info->widening == 2) {
        anvil_strbuf_appendf(p->code, "\tpcmpeqw %%%s, %%%s\n\tpsrlw $15, %%%s\n\tpmaddwd %%%s, %%%s\n", t2, t2, t2, t2, t1);
    } else {
        /* pmaddubsw multiplies its destination's unsigned bytes (ones)
         * by the source's signed ones (x). */
        anvil_strbuf_appendf(p->code, "\tpcmpeqb %%%s, %%%s\n\tpabsb %%%s, %%%s\n\tpmaddubsw %%%s, %%%s\n", t2, t2, t2, t2, t1, t2);
        anvil_strbuf_appendf(p->code, "\tmovdqa %%%s, %%%s\n\tpcmpeqw %%%s, %%%s\n\tpsrlw $15, %%%s\n\tpmaddwd %%%s, %%%s\n", t2, t1, t2, t2, t2, t2,
                             t1);
    }
    if (strcmp(dst, a) != 0)
        anvil_strbuf_appendf(p->code, "\tmovdqa %%%s, %%%s\n", a, dst);
    anvil_strbuf_appendf(p->code, "\t%s %%%s, %%%s\n", info->imm == 64 ? "paddq" : "paddd", t1, dst);
}

/* dst = a * b + c with an accumulating FMA3 form (`OP b, a, dst` computes
 * dst = a * b + dst): c goes into dst first, through the temp when dst
 * holds a or b. */
static void sse_fma(x86_sse_printer_t *p, const char *mnemonic, anvil_mir_vreg_t def, anvil_mir_vreg_t a, anvil_mir_vreg_t b, anvil_mir_vreg_t c)
{
    const char *dst = sse_xmm(p, def), *x = sse_xmm(p, a), *y = sse_xmm(p, b), *z = sse_xmm(p, c);
    if (p->failed)
        return;
    const char *work = strcmp(dst, z) == 0 || (strcmp(dst, x) != 0 && strcmp(dst, y) != 0) ? dst : p->temp;
    if (strcmp(work, z) != 0)
        anvil_strbuf_appendf(p->code, "\tmovaps %%%s, %%%s\n", z, work);
    anvil_strbuf_appendf(p->code, "\t%s %%%s, %%%s, %%%s\n", mnemonic, y, x, work);
    if (strcmp(work, dst) != 0)
        anvil_strbuf_appendf(p->code, "\tmovaps %%%s, %%%s\n", work, dst);
}

void x86_sse_emit_vector(x86_sse_printer_t *p, const anvil_vector_isa_t *isa, size_t index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(p->mir, index, 0);
    anvil_mir_vreg_t rhs = info->num_uses > 1 ? anvil_mir_get_instr_use(p->mir, index, 1) : ANVIL_MIR_NO_VREG;
    if (!isa || lhs == ANVIL_MIR_NO_VREG) {
        p->failed = true;
        return;
    }
    if (info->ir_op == ANVIL_OP_SPLAT) {
        sse_splat(p, isa, info, lhs);
        return;
    }
    if (info->ir_op == ANVIL_OP_SELECT) {
        sse_select(p, index, info);
        return;
    }
    if (info->ir_op == ANVIL_OP_FMA) {
        sse_fma(p, isa->mnemonic, info->def, lhs, rhs, anvil_mir_get_instr_use(p->mir, index, 2));
        return;
    }
    if (anvil_op_is_reduction(info->ir_op)) {
        sse_reduce(p, index, isa, info, lhs);
        return;
    }
    if (info->widening) {
        sse_accumulate(p, info, lhs, rhs);
        return;
    }
    if (info->num_uses == 1) {
        /* A unary operation writes its destination from one source (a
         * scalar one only its low lane: zeroing the rest first breaks the
         * dependency on the old value). */
        const char *dst = sse_xmm(p, info->def), *source = sse_xmm(p, lhs);
        if (p->failed)
            return;
        if (info->op == ANVIL_MIR_OP_SCALAR && strcmp(dst, source) != 0)
            anvil_strbuf_appendf(p->code, "\txorps %%%s, %%%s\n", dst, dst);
        anvil_strbuf_appendf(p->code, "\t%s %%%s, %%%s\n", isa->mnemonic, source, dst);
        return;
    }
    if (rhs == ANVIL_MIR_NO_VREG) {
        p->failed = true;
        return;
    }
    if (info->ir_op == ANVIL_OP_FCMP) {
        sse_fcmp(p, isa, info, lhs, rhs);
        return;
    }
    if (info->ir_op >= ANVIL_OP_CMP_EQ && info->ir_op <= ANVIL_OP_CMP_UGE) {
        sse_int_compare(p, isa, info, lhs, rhs);
        return;
    }
    if (isa->shape == ANVIL_VECTOR_SHAPE_UNIFORM) {
        /* One count for every lane, from its general register into the
         * temp. */
        const anvil_mir_vreg_info_t *count = anvil_mir_get_vreg_info(p->mir, rhs);
        const char *dst = sse_xmm(p, info->def), *a = sse_xmm(p, lhs);
        int phys = sse_phys(p, rhs);
        if (p->failed || !count) {
            p->failed = true;
            return;
        }
        bool wide = count->size_bits > 32;
        const char *source = sse_gpr(p, phys, wide ? 64 : 32);
        if (p->failed)
            return;
        anvil_strbuf_appendf(p->code, "\t%s %%%s, %%%s\n", wide ? "movq" : "movd", source, p->temp);
        if (strcmp(a, dst) != 0)
            anvil_strbuf_appendf(p->code, "\tmovaps %%%s, %%%s\n", a, dst);
        anvil_strbuf_appendf(p->code, "\t%s %%%s, %%%s\n", isa->mnemonic, p->temp, dst);
        return;
    }
    x86_sse_emit_destructive(p, isa->mnemonic, info->def, lhs, rhs, sse_commutative(info->ir_op));
}
