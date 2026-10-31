/* i386 floating point on the x87 FPU, for CPU models without SSE2.
 *
 * FPR assignments name 8-byte home slots in the frame instead of registers;
 * each operation loads its operands onto the x87 stack, computes and stores
 * the result, so the stack is empty between instructions (as the C
 * conventions require at calls) and ST(0) only carries return values.
 * Arithmetic runs at the FPU's precision-control setting; storing to the
 * home slot rounds to the operand's format. */
#include "gas32_internal.h"

#include <stdio.h>

enum {
    X87_HOME_SLOTS = 8,
    X87_SCRATCH = X87_HOME_SLOTS * 8,  /* control words and a 64-bit integer */
    X87_AREA = X87_SCRATCH + 16
};

static const char *suffix(int size)
{
    return size <= 4 ? "s" : "l";
}

/* "-N(%ebp)" for the home slot of an FPR vreg. */
static const char *home(x86_mir_emit_t *emit, anvil_mir_vreg_t vreg, char *buf, size_t len)
{
    int phys = x86_phys_of(emit, vreg);
    if (phys < 0 || phys >= X87_HOME_SLOTS) {
        emit->failed = true;
        return "?";
    }
    snprintf(buf, len, "%d(%%ebp)", -emit->fp_area_off + phys * 8);
    return buf;
}

static int scratch(const x86_mir_emit_t *emit, int byte)
{
    return -emit->fp_area_off + X87_SCRATCH + byte;
}

static bool uses_fp(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_vregs(mir); i++) {
        const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, (anvil_mir_vreg_t)i);
        if (info && info->reg_class == ANVIL_MIR_REG_FPR)
            return true;
    }
    return false;
}

static int frame_bytes(const x86_mir_emit_t *emit)
{
    return uses_fp(emit->mir) ? X87_AREA : 0;
}

static void copy(x86_mir_emit_t *emit, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *dst_info = x86_vreg_info_checked(emit, dst);
    const anvil_mir_vreg_info_t *src_info = x86_vreg_info_checked(emit, src);
    char a[32], b[32];
    if (!dst_info || !src_info)
        return;

    if (dst_info->reg_class == ANVIL_MIR_REG_FPR && src_info->reg_class == ANVIL_MIR_REG_FPR) {
        if (x86_phys_of(emit, dst) == x86_phys_of(emit, src) || emit->failed)
            return;
        int size = x86_size_bytes(dst_info->size_bits);
        const char *from = home(emit, src, a, sizeof(a)), *to = home(emit, dst, b, sizeof(b));
        if (!emit->failed)
            anvil_strbuf_appendf(&emit->code, "\tfld%s %s\n\tfstp%s %s\n", suffix(size), from, suffix(size), to);
    } else if (dst_info->reg_class == ANVIL_MIR_REG_FPR) {
        const char *to = home(emit, dst, b, sizeof(b));
        int phys = x86_phys_of(emit, src);
        if (!emit->failed)
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %s\n", x86_gpr_name(phys, 4), to);
    } else {
        const char *from = home(emit, src, a, sizeof(a));
        int phys = x86_phys_of(emit, dst);
        if (!emit->failed)
            anvil_strbuf_appendf(&emit->code, "\tmovl %s, %%%s\n", from, x86_gpr_name(phys, 4));
    }
}

static void constant(x86_mir_emit_t *emit, anvil_mir_vreg_t dst, uint64_t bits)
{
    int size = x86_fp_size(emit, dst);
    int phys = x86_phys_of(emit, dst);
    if (emit->failed)
        return;
    int offset = -emit->fp_area_off + phys * 8;
    anvil_strbuf_appendf(&emit->code, "\tmovl $%u, %d(%%ebp)\n", (uint32_t)bits, offset);
    if (size > 4)
        anvil_strbuf_appendf(&emit->code, "\tmovl $%u, %d(%%ebp)\n", (uint32_t)(bits >> 32), offset + 4);
}

static void binary(x86_mir_emit_t *emit, anvil_mir_opcode_t op, anvil_mir_vreg_t dst, anvil_mir_vreg_t a, anvil_mir_vreg_t b)
{
    char ha[32], hb[32], hd[32];
    int size = x86_fp_size(emit, dst);
    const char *name = op == ANVIL_MIR_OP_ADD ? "fadd" : op == ANVIL_MIR_OP_SUB ? "fsub" : op == ANVIL_MIR_OP_MUL ? "fmul"
                     : op == ANVIL_MIR_OP_DIV || op == ANVIL_MIR_OP_FDIV ? "fdiv" : NULL;
    const char *ma = home(emit, a, ha, sizeof(ha)), *mb = home(emit, b, hb, sizeof(hb)), *md = home(emit, dst, hd, sizeof(hd));
    if (!name)
        emit->failed = true;
    if (emit->failed)
        return;
    anvil_strbuf_appendf(&emit->code, "\tfld%s %s\n\t%s%s %s\n\tfstp%s %s\n", suffix(size), ma, name, suffix(size), mb, suffix(size), md);
}

/* ST(0) instructions: load, operate, store. */
static void apply(x86_mir_emit_t *emit, const char *mnemonic, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    char hs[32], hd[32];
    int size = x86_fp_size(emit, dst);
    const char *ms = home(emit, src, hs, sizeof(hs)), *md = home(emit, dst, hd, sizeof(hd));
    if (!emit->failed)
        anvil_strbuf_appendf(&emit->code, "\tfld%s %s\n\t%s\n\tfstp%s %s\n", suffix(size), ms, mnemonic, suffix(size), md);
}

static void unary(x86_mir_emit_t *emit, anvil_mir_opcode_t op, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    if (op != ANVIL_MIR_OP_NEG && op != ANVIL_MIR_OP_FABS)
        emit->failed = true;
    else
        apply(emit, op == ANVIL_MIR_OP_NEG ? "fchs" : "fabs", dst, src);
}

static void compare(x86_mir_emit_t *emit, anvil_mir_vreg_t a, anvil_mir_vreg_t b)
{
    char ha[32], hb[32];
    int size = x86_fp_size(emit, a);
    const char *ma = home(emit, a, ha, sizeof(ha)), *mb = home(emit, b, hb, sizeof(hb));
    if (emit->failed)
        return;
    /* ST(0) = a, ST(1) = b. */
    anvil_strbuf_appendf(&emit->code, "\tfld%s %s\n\tfld%s %s\n", suffix(size), mb, suffix(size), ma);
    if (emit->features & ANVIL_FEATURE_X86_CMOV) {
        /* P6 and later: the flags directly. */
        anvil_strbuf_append(&emit->code, "\tfucomip %st(1), %st\n\tfstp %st(0)\n");
    } else {
        /* C0/C2/C3 reach CF/PF/ZF through AH; EAX is preserved (MOV
         * leaves the flags alone). */
        anvil_strbuf_appendf(&emit->code, "\tfucompp\n\tmovl %%eax, %d(%%ebp)\n\tfnstsw %%ax\n\tsahf\n\tmovl %d(%%ebp), %%eax\n", scratch(emit, 8),
                             scratch(emit, 8));
    }
}

/* Truncating x87 store of ST(0): round toward zero for the duration. */
static void store_integer_truncated(x86_mir_emit_t *emit, const char *op, int at)
{
    anvil_strbuf_appendf(&emit->code, "\tfnstcw %d(%%ebp)\n\tfnstcw %d(%%ebp)\n\torw $0x0c00, %d(%%ebp)\n\tfldcw %d(%%ebp)\n", scratch(emit, 0),
                         scratch(emit, 2), scratch(emit, 2), scratch(emit, 2));
    anvil_strbuf_appendf(&emit->code, "\t%s %d(%%ebp)\n\tfldcw %d(%%ebp)\n", op, at, scratch(emit, 0));
}

static void convert(x86_mir_emit_t *emit, anvil_mir_opcode_t op, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    char hs[32], hd[32];
    switch (op) {
    case ANVIL_MIR_OP_SITOFP:
    case ANVIL_MIR_OP_UITOFP: {
        int size = x86_fp_size(emit, dst);
        const char *s = x86_gpr_name(x86_phys_of(emit, src), 4), *md = home(emit, dst, hd, sizeof(hd));
        if (emit->failed)
            break;
        /* Unsigned: the 64-bit value (0, src), exact on the FPU. */
        anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %d(%%ebp)\n", s, scratch(emit, 8));
        if (op == ANVIL_MIR_OP_UITOFP)
            anvil_strbuf_appendf(&emit->code, "\tmovl $0, %d(%%ebp)\n\tfildll %d(%%ebp)\n", scratch(emit, 12), scratch(emit, 8));
        else
            anvil_strbuf_appendf(&emit->code, "\tfildl %d(%%ebp)\n", scratch(emit, 8));
        anvil_strbuf_appendf(&emit->code, "\tfstp%s %s\n", suffix(size), md);
        break;
    }
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI: {
        int size = x86_fp_size(emit, src);
        const char *ms = home(emit, src, hs, sizeof(hs)), *d = x86_gpr_name(x86_phys_of(emit, dst), 4);
        if (emit->failed)
            break;
        /* Unsigned: the low word of the 64-bit conversion. */
        anvil_strbuf_appendf(&emit->code, "\tfld%s %s\n", suffix(size), ms);
        store_integer_truncated(emit, op == ANVIL_MIR_OP_FPTOUI ? "fistpll" : "fistpl", scratch(emit, 8));
        anvil_strbuf_appendf(&emit->code, "\tmovl %d(%%ebp), %%%s\n", scratch(emit, 8), d);
        break;
    }
    case ANVIL_MIR_OP_FPEXT:
    case ANVIL_MIR_OP_FPTRUNC: {
        const char *ms = home(emit, src, hs, sizeof(hs)), *md = home(emit, dst, hd, sizeof(hd));
        if (!emit->failed)
            anvil_strbuf_appendf(&emit->code, "\tfld%s %s\n\tfstp%s %s\n", op == ANVIL_MIR_OP_FPEXT ? "s" : "l", ms, op == ANVIL_MIR_OP_FPEXT ? "l" : "s",
                                 md);
        break;
    }
    default:
        emit->failed = true;
        break;
    }
}

static void load(x86_mir_emit_t *emit, anvil_mir_vreg_t dst, const char *mem)
{
    char hd[32];
    int size = x86_fp_size(emit, dst);
    const char *md = home(emit, dst, hd, sizeof(hd));
    if (!emit->failed)
        anvil_strbuf_appendf(&emit->code, "\tfld%s %s\n\tfstp%s %s\n", suffix(size), mem, suffix(size), md);
}

static void store(x86_mir_emit_t *emit, anvil_mir_vreg_t src, const char *mem)
{
    char hs[32];
    int size = x86_fp_size(emit, src);
    const char *ms = home(emit, src, hs, sizeof(hs));
    if (!emit->failed)
        anvil_strbuf_appendf(&emit->code, "\tfld%s %s\n\tfstp%s %s\n", suffix(size), ms, suffix(size), mem);
}

static void push_result(x86_mir_emit_t *emit, anvil_mir_vreg_t src)
{
    char hs[32];
    int size = x86_fp_size(emit, src);
    const char *ms = home(emit, src, hs, sizeof(hs));
    if (!emit->failed)
        anvil_strbuf_appendf(&emit->code, "\tfld%s %s\n", suffix(size), ms);
}

static void pop_result(x86_mir_emit_t *emit, anvil_mir_vreg_t dst)
{
    char hd[32];
    int size = x86_fp_size(emit, dst);
    const char *md = home(emit, dst, hd, sizeof(hd));
    if (!emit->failed)
        anvil_strbuf_appendf(&emit->code, "\tfstp%s %s\n", suffix(size), md);
}

const x86_fp_unit_t x86_fp_x87_unit = {
    .name = "x87",
    .frame_bytes = frame_bytes,
    .copy = copy,
    .constant = constant,
    .binary = binary,
    .unary = unary,
    .apply = apply,
    .compare = compare,
    .convert = convert,
    .load = load,
    .store = store,
    .push_result = push_result,
    .pop_result = pop_result,
};
